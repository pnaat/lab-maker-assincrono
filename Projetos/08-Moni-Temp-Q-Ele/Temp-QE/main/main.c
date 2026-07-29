#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_log.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

static const char *TAG = "NTC10K";

// --- MQTT ---
#define MQTT_TOPIC_EVENTO       "enchimento/evento"
#define MQTT_QOS                1

// === Parâmetros do sensor/divisor ===
#define R_FIXED_OHMS      10000.0f      // 10k
#define R0_OHMS           10000.0f      // 10k @ 25°C
#define T0_K              298.15f       // 25°C em Kelvin
#define BETA_K            3950.0f       // ajuste p/ seu NTC: 3435, 3950, etc.
#define SAMPLES           64            // multisampling

// === ADC config ===
#define ADC_ATTEN         ADC_ATTEN_DB_12
#define ADC_WIDTH         ADC_WIDTH_BIT_12

static esp_adc_cal_characteristics_t adc_chars;

static esp_mqtt_client_handle_t s_mqtt = NULL;

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

static bool get_adc_info_from_channel_num(int num, adc1_channel_t *channel)
{
    if(num >= 0 && num < ADC1_CHANNEL_MAX) {
         *channel = (adc1_channel_t) num;
         return true;
    }
    return false;
}

static uint32_t read_adc_mv(void)
{
    uint32_t adc_reading = 0;
    adc1_channel_t channel = 0;
    if(!get_adc_info_from_channel_num(CONFIG_ESP_PTC_ADC_CHANNEL, &channel))
        return 0;

    for (int i = 0; i < SAMPLES; i++) {
        adc_reading += adc1_get_raw(channel);
    }
    adc_reading /= SAMPLES;

    uint32_t voltage_mv = 0;
    voltage_mv = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
    return voltage_mv; // em mV
}

static void mqtt_publicar_data(float value)
{
    if (!s_mqtt) return;

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{"
          "\"evento\":\"temperatura\","
          "\"valor\":%.2f"
        "}",
        value);

    int msg_id = esp_mqtt_client_publish(s_mqtt, MQTT_TOPIC_EVENTO, payload, 0, MQTT_QOS, 0);
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "MQTT publicado em %s: %s", MQTT_TOPIC_EVENTO, payload);
    } else {
        ESP_LOGW(TAG, "Falha ao publicar MQTT.");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // Wi-Fi + MQTT
    wifi_init_sta();
    // Espera Wi-Fi para iniciar MQTT
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    mqtt_start();

    // 1) Configura ADC
    adc1_config_width(ADC_WIDTH);

    adc1_channel_t channel = 0;
    if(!get_adc_info_from_channel_num(CONFIG_ESP_PTC_ADC_CHANNEL, &channel))
    {
        ESP_LOGE(TAG, "invalid ADC channel selected!");
        while(1){}
    }

    adc1_config_channel_atten(channel, ADC_ATTEN);

    // 2) Caracteriza ADC com eFuse/curva
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH, 1100, &adc_chars);
    // A Vref "1100" é só default; a correta vem do eFuse se disponível.
    switch (val_type) {
        case ESP_ADC_CAL_VAL_EFUSE_TP:
            ESP_LOGI(TAG, "Calib: Two Point (eFuse)");
            break;
        case ESP_ADC_CAL_VAL_EFUSE_VREF:
            ESP_LOGI(TAG, "Calib: eFuse Vref");
            break;
        default:
            ESP_LOGW(TAG, "Calib: Default Vref (1100 mV) – menos preciso");
            break;
    }

    TickType_t send = xTaskGetTickCount();

    // 3) Loop de leitura
    while (1) {
        // 3.1) Lê tensão no nó do divisor
        uint32_t v_adc_mv = read_adc_mv();        // mV no pino ADC
        float Vadc = v_adc_mv / 1000.0f;          // em Volts

        // 3.2) Tensão de referência do divisor (topo = 3V3 real)
        // Melhor: medir 3V3 com outro canal e usar esse valor.
        const float Vref = 3.30f;                 // ajuste p/ seu board

        // Proteção contra limites
        if (Vadc < 0.001f) Vadc = 0.001f;
        if (Vadc > Vref - 0.001f) Vadc = Vref - 0.001f;

        // 3.3) Converte para Rntc
        float Rntc = R_FIXED_OHMS * (Vadc / (Vref - Vadc));

        // 3.4) Beta equation -> Temperatura (Kelvin)
        float invT = (1.0f / T0_K) + (1.0f / BETA_K) * logf(Rntc / R0_OHMS);
        float T_k = 1.0f / invT;
        float T_c = T_k - 273.15f;

        ESP_LOGI(TAG, "Vadc=%.3f V, Rntc=%.1f ohms, T=%.2f °C", Vadc, Rntc, T_c);

        if((xTaskGetTickCount() - send) > pdMS_TO_TICKS(5000))
        {
            mqtt_publicar_data(T_c);
            send = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
