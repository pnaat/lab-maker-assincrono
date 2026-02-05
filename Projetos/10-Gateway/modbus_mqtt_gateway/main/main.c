#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "modbus_rtu.h"

#define WIFI_SSID "SUA_REDE"
#define WIFI_PASS "SENHA"

#define MQTT_URI "mqtt://192.168.1.10:1883"
#define MQTT_CLIENT_ID "esp32s3-modbus-gw"
#define TOPIC_BASE "factory/linha1"

#define POLL_INTERVAL_MS 1000

static const char *TAG = "gateway";

typedef struct {
    uint8_t slave;
    uint16_t addr;
    uint16_t qty;
    const char *topic;
    uint16_t scale;
    uint8_t func; // 3 holding, 4 input
} mb_query_t;

static const mb_query_t s_queries[] = {
    { 1, 0x0000, 2, "m1/holding/40001", 1, 3 },
    { 2, 0x0000, 4, "m2/holding/40001", 10, 3 },
    { 1, 0x1000, 2, "m1/input/30001", 1, 4 },
};
static const int s_qcount = sizeof(s_queries) / sizeof(s_queries[0]);

static esp_mqtt_client_handle_t mqtt = NULL;

// Wi‑Fi handler
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
}

static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);

    wifi_config_t wcfg = { 0 };
    strcpy((char*)wcfg.sta.ssid, WIFI_SSID);
    strcpy((char*)wcfg.sta.password, WIFI_PASS);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    // Converte o ponteiro de dados do evento
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT", "Conectado ao broker");
            // Ex.: publicar um status inicial, se quiser:
            // esp_mqtt_client_publish(client, "gateway/status", "{\"status\":\"online\"}", 0, 0, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW("MQTT", "Desconectado do broker");
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW("MQTT", "Erro MQTT");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI("MQTT", "Inscrito: msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            // Opcional: logs de publicação
            // ESP_LOGD("MQTT", "Publicado: msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            // Aqui você trataria comandos recebidos (ex.: write via MQTT)
            ESP_LOGI("MQTT", "DATA topic=%.*s data=%.*s",
                     event->topic_len, event->topic,
                     event->data_len, event->data);
            break;

        default:
            // ESP_LOGD("MQTT", "Evento id: %d", event->event_id);
            break;
    }
}


static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.client_id = MQTT_CLIENT_ID
    };
    mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt);
}

static void publish_values(const char *topic_rel,
                           uint16_t *regs, int qty, int scale)
{
    char topic[128];
    char payload[256];

    snprintf(topic, sizeof(topic), "%s/%s", TOPIC_BASE, topic_rel);

    int off = 0;
    off += snprintf(payload + off, sizeof(payload) - off,
                    "{ \"ts\":%lu, \"values\":[",
                    (unsigned long)(esp_timer_get_time()/1000000ULL));

    for (int i = 0; i < qty; i++) {
        float v = (scale > 1) ? (regs[i] / (float)scale) : regs[i];
        off += snprintf(payload + off, sizeof(payload) - off,
                        (i == 0 ? "%0.3f" : ",%0.3f"), v);
    }

    snprintf(payload + off, sizeof(payload) - off, "] }");

    esp_mqtt_client_publish(mqtt, topic, payload, 0, 0, 0);
}

static void poll_task(void *arg)
{
    uint16_t regs[32];

    while (1) {
        for (int i = 0; i < s_qcount; i++) {
            const mb_query_t *q = &s_queries[i];
            esp_err_t ok;

            if (q->func == 3)
                ok = modbus_rtu_read_holding(q->slave, q->addr, q->qty, regs, 300);
            else
                ok = modbus_rtu_read_input(q->slave, q->addr, q->qty, regs, 300);

            if (ok == ESP_OK)
                publish_values(q->topic, regs, q->qty, q->scale);

            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    mqtt_start();

    modbus_rtu_init();

    xTaskCreate(poll_task, "poll", 4096, NULL, 5, NULL);
}