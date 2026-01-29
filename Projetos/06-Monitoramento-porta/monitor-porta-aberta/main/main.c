// main.c
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "mqtt_client.h"

// ===================== CONFIGURAÇÕES ===========================

// --- Wi-Fi ---
#define WIFI_SSID           "SUA_REDE_WIFI"
#define WIFI_PASS           "SUA_SENHA_WIFI"
#define WIFI_MAX_RETRY      5

// --- MQTT ---
#define MQTT_BROKER_URI     "mqtt://192.168.0.100:1883"
#define MQTT_TOPIC_STATS    "coldroom/door/stats"
#define MQTT_TOPIC_ALARM    "coldroom/door/alarm"

// --- GPIOs (AJUSTAR) ---
#define GPIO_DOOR           7   // Entrada do fim de curso (NF -> GND)
#define DOOR_ACTIVE_HIGH    1   // 1: nível alto = ABERTA; 0: invertido
#define GPIO_BUZZER         8   // Saída para buzzer (LEDC)

// --- Debounce e Alarme ---
#define DOOR_DEBOUNCE_MS    30
#define OPEN_ALARM_SEC      60  // <<< defina o limite desejado (em segundos)

// --- Buzzer (LEDC) ---
#define BUZZER_FREQ_HZ      2000
#define BUZZER_DUTY_PCT     40
#define BUZZER_BEEP_MS_ON   400
#define BUZZER_BEEP_MS_OFF  400

// ===================== VARIÁVEIS GLOBAIS =======================
static const char *TAG = "COLDROOM";

static TimerHandle_t s_debounce_timer = NULL;
static TimerHandle_t s_open_alarm_timer = NULL;
static TimerHandle_t s_buzzer_beep_timer = NULL;

static volatile int  s_door_level_stable = 0;
static volatile bool s_door_is_open = false;
static volatile int64_t s_door_open_start_us = 0;

static volatile bool s_alarm_active = false;

static esp_mqtt_client_handle_t s_mqtt = NULL;
static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static uint32_t s_aberturas_dia = 0;
static uint64_t s_tempo_aberto_ms_dia = 0;
static int      s_current_ymd = 0; // YYYYMMDD guardado p/ rollover

// ===================== WIFI / SNTP / MQTT ======================
static void sync_time(void)
{
    // Fuso horário Brasil (sem DST atual): UTC-3
    setenv("TZ", "<-03>3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // aguarda sincronizar (não bloqueante absoluto, apenas best-effort)
    for (int i = 0; i < 10; i++) {
        time_t now = 0; time(&now);
        if (now > 8 * 3600) break; // heurística: epoch válido
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    static int s_retry_num = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi desconectado, tentando reconectar... (%d)", s_retry_num);
        } else {
            ESP_LOGE(TAG, "Falha ao conectar no WiFi");
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando ao WiFi SSID:%s", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado");
            break;
        default:
            break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.keepalive = 60,
    };
    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

static void mqtt_publish(const char *topic, const char *payload)
{
    if (!s_mqtt) return;
    int msg_id = esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "MQTT publish (id=%d) topic=%s payload=%s", msg_id, topic, payload);
}

// ===================== UTILITÁRIOS DE DATA =====================
static int ymd_from_time(time_t now, char *out_day_str, size_t out_sz)
{
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    if (out_day_str && out_sz >= 11) {
        snprintf(out_day_str, out_sz, "%04d-%02d-%02d",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
    }
    return (tm_info.tm_year + 1900) * 10000 + (tm_info.tm_mon + 1) * 100 + tm_info.tm_mday;
}

static void check_day_rollover_and_reset_if_needed(void)
{
    time_t now = 0; time(&now);
    char day_str[11] = {0};
    int ymd = ymd_from_time(now, day_str, sizeof(day_str));
    if (s_current_ymd == 0) {
        s_current_ymd = ymd; // primeira inicialização
        return;
    }
    if (ymd != s_current_ymd) {
        ESP_LOGI(TAG, "Virou o dia (%d -> %d). Resetando contadores.", s_current_ymd, ymd);
        s_current_ymd = ymd;
        s_aberturas_dia = 0;
        s_tempo_aberto_ms_dia = 0;
    }
}

// ======================= BUZZER (LEDC) =========================
static void buzzer_on(void)
{
    // duty em 10-bit: duty = pct * (2^bit - 1) / 100
    const int duty = (BUZZER_DUTY_PCT * 8191) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_off(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_beep_timer_cb(TimerHandle_t xTimer)
{
    static bool s_on = false;
    if (s_alarm_active) {
        if (s_on) {
            buzzer_off();
            xTimerChangePeriod(s_buzzer_beep_timer, pdMS_TO_TICKS(BUZZER_BEEP_MS_OFF), 0);
        } else {
            buzzer_on();
            xTimerChangePeriod(s_buzzer_beep_timer, pdMS_TO_TICKS(BUZZER_BEEP_MS_ON), 0);
        }
        s_on = !s_on;
        xTimerStart(s_buzzer_beep_timer, 0);
    } else {
        s_on = false;
        buzzer_off();
    }
}

static void buzzer_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .gpio_num = GPIO_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0, // começa desligado
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    s_buzzer_beep_timer = xTimerCreate("buzzer_beep", pdMS_TO_TICKS(BUZZER_BEEP_MS_ON), pdFALSE, NULL, buzzer_beep_timer_cb);
}

// ==================== LÓGICA DA PORTA ==========================
static void publish_stats_on_open(void)
{
    time_t now = 0; time(&now);
    char dia[11] = {0};
    ymd_from_time(now, dia, sizeof(dia));

    // Monta JSON simples (sem lib externa)
    char payload[160] = {0};
    
    snprintf(payload, sizeof(payload),
         "{\"dia\":\"%s\",\"aberturas_dia\":%" PRIu32 ",\"tempo_aberto_ms_dia\":%" PRIu64 ",\"evento\":\"abriu\"}",
         dia, (uint32_t)s_aberturas_dia, (uint64_t)s_tempo_aberto_ms_dia);

    mqtt_publish(MQTT_TOPIC_STATS, payload);
}

static void publish_alarm_open_exceeded(int limit_s, int open_s)
{
    time_t now = 0; time(&now);
    char dia[11] = {0};
    ymd_from_time(now, dia, sizeof(dia));

    char payload[160] = {0};
    snprintf(payload, sizeof(payload),
             "{\"dia\":\"%s\",\"alarme\":\"porta_aberta_acima_limite\",\"limite_s\":%d,\"aberta_s\":%d}",
             dia, limit_s, open_s);
    mqtt_publish(MQTT_TOPIC_ALARM, payload);
}

static void door_open_alarm_cb(TimerHandle_t xTimer)
{
    if (s_door_is_open && !s_alarm_active) {
        s_alarm_active = true;
        ESP_LOGE(TAG, "ALARME: Porta aberta acima de %d s!", OPEN_ALARM_SEC);
        // Ativa padrão de beep intermitente
        xTimerStop(s_buzzer_beep_timer, 0);
        xTimerChangePeriod(s_buzzer_beep_timer, pdMS_TO_TICKS(1), 0); // dispara já
        xTimerStart(s_buzzer_beep_timer, 0);

        int open_s = (int)((esp_timer_get_time() - s_door_open_start_us) / 1000000);
        publish_alarm_open_exceeded(OPEN_ALARM_SEC, open_s);
    }
}

static void door_update_state_from_level(int level)
{
    bool open = DOOR_ACTIVE_HIGH ? (level != 0) : (level == 0);
    if (open == s_door_is_open) {
        return; // sem mudança real
    }

    s_door_is_open = open;

    if (open) {
        // --- ABRIU ---
        check_day_rollover_and_reset_if_needed();

        s_door_open_start_us = esp_timer_get_time();
        s_aberturas_dia++;                  // soma a abertura do dia
        ESP_LOGW(TAG, "PORTA ABERTA (aberturas_dia=%u)", s_aberturas_dia);

        // agenda alarme
        if (s_open_alarm_timer) {
            xTimerStop(s_open_alarm_timer, 0);
            xTimerChangePeriod(s_open_alarm_timer, pdMS_TO_TICKS(OPEN_ALARM_SEC * 1000), 0);
            xTimerStart(s_open_alarm_timer, 0);
        }

        // envia os dados acumulados até agora
        publish_stats_on_open();

    } else {
        // --- FECHOU ---
        if (s_open_alarm_timer) {
            xTimerStop(s_open_alarm_timer, 0);
        }

        int64_t dur_ms = (esp_timer_get_time() - s_door_open_start_us) / 1000;
        if (dur_ms > 0) {
            s_tempo_aberto_ms_dia += (uint64_t)dur_ms;
        }

        // se estava em alarme, desativa buzzer
        if (s_alarm_active) {
            s_alarm_active = false;
            buzzer_off();
        }

        ESP_LOGI(TAG, "PORTA FECHADA (duracao_ms=%" PRId64 ", total_ms_dia=%" PRIu64 ")",
                 dur_ms, s_tempo_aberto_ms_dia);
    }
}

// ISR: aciona debounce
static void IRAM_ATTR door_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_debounce_timer) {
        xTimerResetFromISR(s_debounce_timer, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void debounce_timer_cb(TimerHandle_t xTimer)
{
    int level = gpio_get_level(GPIO_DOOR);
    if (level != s_door_level_stable) {
        s_door_level_stable = level;
        door_update_state_from_level(level);
    }
}

static void door_gpio_init(void)
{
    // Entrada com pull-up; interrupção em ambos os flancos
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_DOOR,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // leitura inicial
    s_door_level_stable = gpio_get_level(GPIO_DOOR);
    s_door_is_open = DOOR_ACTIVE_HIGH ? (s_door_level_stable != 0) : (s_door_level_stable == 0);
    if (s_door_is_open) s_door_open_start_us = esp_timer_get_time();

    // timers
    s_debounce_timer = xTimerCreate("debounce", pdMS_TO_TICKS(DOOR_DEBOUNCE_MS), pdFALSE, NULL, debounce_timer_cb);
    s_open_alarm_timer = xTimerCreate("open_alarm", pdMS_TO_TICKS(OPEN_ALARM_SEC * 1000), pdFALSE, NULL, door_open_alarm_cb);

    // ISR
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_DOOR, door_isr_handler, NULL);
}

// ======================== APP MAIN =============================
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // WiFi + MQTT
    wifi_init_sta();
    sync_time();
    mqtt_start();

    // Buzzer
    buzzer_init();

    // Porta
    door_gpio_init();

    // Inicializa "data do dia" para rollover
    time_t now = 0; time(&now);
    s_current_ymd = ymd_from_time(now, NULL, 0);

    ESP_LOGI(TAG, "Sistema iniciado. Limite de alarme: %d s", OPEN_ALARM_SEC);

    // Nada no loop — tudo em callbacks/timers
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}