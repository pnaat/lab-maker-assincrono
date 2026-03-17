// main.c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "mqtt_client.h"

// ===================== CONFIGURAÇÕES =====================

// --- MQTT ---
#define MQTT_TOPIC_EVENTO       "fabrica/linha1/qualidade/enchimento/evento"
#define MQTT_QOS                1

// --- Pinos (ajuste conforme seu Heltec ESP32-S3 V3) ---
// CONFIG_GPIO_TRIG Saída   -> TRIG do HC-SR04
// CONFIG_GPIO_ECHO Entrada <- ECHO (via divisor/level shifter!)

// --- Ultrassônico & amostragem ---
#define TEMP_C                  25.0f   // temperatura para compensar velocidade do som
#define ECHO_TIMEOUT_US         30000   // timeout 30 ms (≈ 5 m ida/volta) para o HC-SR04
#define SAMPLE_PERIOD_MS        20      // período de leitura
#define PRESENCE_THRESHOLD_MM   220     // distância abaixo da qual consideramos "lata presente"
#define GAP_MS                  120     // janela sem presença para fechar a medição
#define MAX_SAMPLES_PER_CAN     64      // max de amostras por lata para a mediana

// --- Critérios de qualidade ---
#define DIST_ALVO_MM            45      // distância alvo sensor->líquido (mm)
#define TOLERANCIA_MM           3       // tolerância (mm)

// --- Sinalização / Atuação ---
#define ALERTA_LED_MS           400     // tempo com LED aceso na rejeição
#define RELE_PULSO_MS           150     // pulso do pistão (rele)

// ===================== GLOBAIS =====================
static const char *TAG = "QC_ENCH";

static esp_mqtt_client_handle_t s_mqtt = NULL;

static volatile uint32_t g_total_passagens = 0;
static volatile uint32_t g_total_fora_nivel = 0;

// Evento para os atuadores (evita bloquear a task de medição)
typedef struct {
    uint32_t dist_mm;
    int32_t  desvio_mm;
} evento_rejeicao_t;

static QueueHandle_t s_q_atuador = NULL;

// ===================== WIFI =====================

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi desconectado, tentando reconectar…");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi conectado e IP obtido.");
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ===================== MQTT =====================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado.");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado.");
            break;
        default:
            break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_ESP_MQTT_URL,
        .credentials.username = CONFIG_ESP_MQTT_USER,
        .credentials.authentication.password = CONFIG_ESP_MQTT_PASS,
        .session.disable_clean_session = false,
        .session.keepalive = 30,
    };
    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

static void mqtt_publicar_rejeicao(uint32_t dist_mm, int32_t desvio_mm)
{
    if (!s_mqtt) return;

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{"
          "\"evento\":\"fora_nivel\","
          "\"total_passagens\":%lu,"
          "\"total_fora\":%lu,"
          "\"desvio_mm\":%d"
        "}",
        g_total_passagens, g_total_fora_nivel, (int)desvio_mm);

    int msg_id = esp_mqtt_client_publish(s_mqtt, MQTT_TOPIC_EVENTO, payload, 0, MQTT_QOS, 0);
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "MQTT publicado em %s: %s", MQTT_TOPIC_EVENTO, payload);
    } else {
        ESP_LOGW(TAG, "Falha ao publicar MQTT.");
    }
}

// ===================== HC-SR04 =====================

static esp_err_t hcsr04_measure_once(uint32_t *distance_mm)
{
    if (!distance_mm) return ESP_ERR_INVALID_ARG;

    // Trigger de 10 us
    gpio_set_level(CONFIG_GPIO_TRIG, 0);
    esp_rom_delay_us(2);
    gpio_set_level(CONFIG_GPIO_TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level(CONFIG_GPIO_TRIG, 0);

    // Espera subida
    int64_t t_wait = esp_timer_get_time();
    while (gpio_get_level(CONFIG_GPIO_ECHO) == 0) {
        if ((esp_timer_get_time() - t_wait) > ECHO_TIMEOUT_US) {
            return ESP_ERR_TIMEOUT;
        }
    }

    // Mede largura do pulso alto
    int64_t t_start = esp_timer_get_time();
    while (gpio_get_level(CONFIG_GPIO_ECHO) == 1) {
        if ((esp_timer_get_time() - t_start) > ECHO_TIMEOUT_US) {
            return ESP_ERR_TIMEOUT;
        }
    }
    int64_t t_end = esp_timer_get_time();

    uint32_t pulse_us = (uint32_t)(t_end - t_start);

    // Velocidade do som (m/s) aproximada por temperatura
    float c_ms = 331.3f + 0.606f * TEMP_C;           // m/s
    float mm_per_us = (c_ms * 1000.0f) / 1e6f;       // mm/us
    float dmm = (pulse_us * mm_per_us) / 2.0f;       // divide por 2 ida/volta

    if (dmm < 0.0f) dmm = 0.0f;
    *distance_mm = (uint32_t)(dmm + 0.5f);
    return ESP_OK;
}

static uint16_t median_u16(uint16_t *buf, size_t n)
{
    if (n == 0) return 0;
    // cópia para ordenar
    uint16_t tmp[MAX_SAMPLES_PER_CAN];
    if (n > MAX_SAMPLES_PER_CAN) n = MAX_SAMPLES_PER_CAN;
    for (size_t i = 0; i < n; ++i) tmp[i] = buf[i];

    // insertion sort (n pequeno)
    for (size_t i = 1; i < n; ++i) {
        uint16_t key = tmp[i];
        size_t j = i;
        while (j > 0 && tmp[j-1] > key) {
            tmp[j] = tmp[j-1];
            --j;
        }
        tmp[j] = key;
    }
    return tmp[n/2];
}

// ===================== ATUADORES =====================

static void atuador_task(void *arg)
{
    evento_rejeicao_t evt;
    for (;;) {
        if (xQueueReceive(s_q_atuador, &evt, portMAX_DELAY) == pdTRUE) {
            // LED ON
            gpio_set_level(CONFIG_GPIO_LED, 1);

            // Pulso no relé
            gpio_set_level(CONFIG_GPIO_RELE, 1);
            vTaskDelay(pdMS_TO_TICKS(RELE_PULSO_MS));
            gpio_set_level(CONFIG_GPIO_RELE, 0);

            // Mantém LED aceso pelo restante do tempo
            if (ALERTA_LED_MS > RELE_PULSO_MS) {
                vTaskDelay(pdMS_TO_TICKS(ALERTA_LED_MS - RELE_PULSO_MS));
            }
            gpio_set_level(CONFIG_GPIO_LED, 0);
        }
    }
}

// ===================== LÓGICA DE MEDIÇÃO/CONTAGEM =====================

static void medicao_task(void *arg)
{
    bool presente = false;
    int64_t ultimo_ts_presenca_ms = 0;

    uint16_t amostras[MAX_SAMPLES_PER_CAN];
    size_t n_amostras = 0;

    for (;;) {
        uint32_t dmm = 0;
        esp_err_t r = hcsr04_measure_once(&dmm);
        if (r == ESP_OK) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool pr = (dmm < PRESENCE_THRESHOLD_MM);

            if (pr) {
                if (!presente) {
                    // borda de subida: início da lata
                    n_amostras = 0;
                }
                presente = true;
                ultimo_ts_presenca_ms = now_ms;

                // coleta amostras enquanto a lata está presente
                if (n_amostras < MAX_SAMPLES_PER_CAN) {
                    amostras[n_amostras++] = (uint16_t)dmm;
                }
            } else {
                // Sem presença
                if (presente && (now_ms - ultimo_ts_presenca_ms) > GAP_MS) {
                    // Lata saiu: fechar medição
                    presente = false;

                    g_total_passagens++;

                    uint32_t dist_final = median_u16(amostras, n_amostras);
                    int32_t desvio = (int32_t)dist_final - (int32_t)DIST_ALVO_MM;
                    bool fora = (desvio > (int32_t)TOLERANCIA_MM) || (desvio < -(int32_t)TOLERANCIA_MM);

                    ESP_LOGI(TAG,
                        "Lata #%u: dist=%u mm, alvo=%u mm, desvio=%d mm, fora=%s",
                        g_total_passagens, dist_final, DIST_ALVO_MM, desvio, fora ? "SIM" : "NAO");

                    if (fora) {
                        g_total_fora_nivel++;

                        // Dispara atuadores pela fila (não bloqueia a medição)
                        evento_rejeicao_t evt = {
                            .dist_mm = dist_final,
                            .desvio_mm = desvio
                        };
                        xQueueSend(s_q_atuador, &evt, 0);

                        // Publica MQTT apenas em fora de nível
                        mqtt_publicar_rejeicao(dist_final, desvio);
                    }

                    // limpa amostras p/ próxima lata
                    n_amostras = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

// ===================== MAIN =====================

static void gpio_init_all(void)
{
    // TRIG
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GPIO_TRIG,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    gpio_set_level(CONFIG_GPIO_TRIG, 0);

    // ECHO
    io.pin_bit_mask = 1ULL << CONFIG_GPIO_ECHO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;  // use divisor/LS externo!
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    // LED
    io.pin_bit_mask = 1ULL << CONFIG_GPIO_LED;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(CONFIG_GPIO_LED, 0);

    // RELE
    io.pin_bit_mask = 1ULL << CONFIG_GPIO_RELE;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(CONFIG_GPIO_RELE, 0);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_init_all();

    // Fila do atuador
    s_q_atuador = xQueueCreate(8, sizeof(evento_rejeicao_t));
    xTaskCreate(atuador_task, "atuador_task", 4096, NULL, 10, NULL);

    // Wi-Fi + MQTT
    wifi_init_sta();
    // Espera Wi-Fi para iniciar MQTT
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    mqtt_start();

    // Tarefa de medição
    xTaskCreate(medicao_task, "medicao_task", 4096, NULL, 12, NULL);
}
