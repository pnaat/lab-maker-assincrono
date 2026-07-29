#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define TOPIC_BASE "factory/linha1"

#define POLL_INTERVAL_MS 1000

#define UART_PORT      UART_NUM_1
#define UART_TX_PIN    6
#define UART_RX_PIN    7
#define UART_RTS_PIN   UART_PIN_NO_CHANGE
#define UART_BAUD      19200
#define UART_PARITY    UART_PARITY_EVEN
#define UART_STOP      UART_STOP_BITS_1

const int WIFI_CONNECTED_BIT = BIT0;
const int WIFI_FAIL_BIT      = BIT1;
const int MQTT_CONNECTED_BIT = BIT2;

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

#if CONFIG_BROKER_CERTIFICATE_OVERRIDDEN == 1
static const uint8_t mqtt_eclipseprojects_io_pem_start[]  = "-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
#else
extern const uint8_t mqtt_eclipseprojects_io_pem_start[]   asm("_binary_mqtt_eclipseprojects_io_pem_start");
#endif
extern const uint8_t mqtt_eclipseprojects_io_pem_end[]   asm("_binary_mqtt_eclipseprojects_io_pem_end");

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
            xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW("MQTT", "Erro MQTT");
            xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
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

esp_mqtt_client_handle_t client = NULL;
static void mqtt_start(void)
{
    ESP_LOGI(TAG, "STARTING MQTT");
    xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);

    esp_mqtt_client_config_t mqttConfig = {0};
    mqttConfig.broker.address.uri = CONFIG_ESP_MQTT_URL;
    mqttConfig.credentials.username = CONFIG_ESP_MQTT_USER;
    mqttConfig.credentials.authentication.password = CONFIG_ESP_MQTT_PASS;
    mqttConfig.broker.verification.certificate = (const char *)mqtt_eclipseprojects_io_pem_start;

    client = esp_mqtt_client_init(&mqttConfig);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

// Wi‑Fi handler
static void _wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "<<<<< WIFI_EVENT_STA_START >>>>>");
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGI(TAG, "<<<<< WIFI_EVENT_STA_DISCONNECTED >>>>>");
    vTaskDelay(200 / portTICK_PERIOD_MS);
    if (s_retry_num < 5) {
      esp_wifi_connect();
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      s_retry_num++;
      ESP_LOGW(TAG, "retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      ESP_LOGE(TAG,"connect to the AP fail");
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "got ip");
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static void wifi_init(void)
{
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
        .ssid = CONFIG_ESP_WIFI_SSID,
        .password = CONFIG_ESP_WIFI_PASS
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "Trying to connect to ap SSID:%s password:%s", CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASS);

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
    * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
    * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected OK");
        mqtt_start();
       // bits = xEventGroupWaitBits(s_wifi_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
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

static uint16_t crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int pos = 0; pos < len; pos++) {
        crc ^= buf[pos];
        for (int i = 0; i < 8; i++) {
            if (crc & 1) { crc >>= 1; crc ^= 0xA001; }
            else crc >>= 1;
        }
    }
    return crc;
}

esp_err_t modbus_rtu_init(void)
{
    ESP_LOGI(TAG, "Inicializando RS-485");

    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_RTS_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_mode(UART_PORT, UART_MODE_RS485_HALF_DUPLEX));
    
    ESP_LOGI(TAG, "RS-485 initialized");
    return ESP_OK;
}

static esp_err_t mb_txrx(uint8_t *tx, int txlen, uint8_t *rx, int *rxlen, int maxrx, uint32_t timeout)
{
    uint16_t crc = crc16(tx, txlen);
    tx[txlen++] = crc & 0xFF;
    tx[txlen++] = crc >> 8;

    uart_flush_input(UART_PORT);
    uart_write_bytes(UART_PORT, (char*)tx, txlen);
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(timeout));

    int total = 0;
    int64_t t_end = esp_timer_get_time() / 1000 + timeout;

    while ((esp_timer_get_time() / 1000) < t_end) {
        int n = uart_read_bytes(UART_PORT, rx + total, maxrx - total, 20 / portTICK_PERIOD_MS);
        if (n > 0) total += n;
        if (total >= 5) break;
    }

    if (total < 5) return ESP_FAIL;

    *rxlen = total;

    uint16_t rcrc = rx[total - 2] | (rx[total - 1] << 8);
    if (crc16(rx, total - 2) != rcrc) return ESP_ERR_INVALID_CRC;

    return ESP_OK;
}

static esp_err_t mb_read_common(uint8_t id, uint8_t func, uint16_t addr,
                                uint16_t qty, uint16_t *out, uint32_t timeout)
{
    uint8_t tx[16], rx[256];
    int rlen;

    tx[0] = id;
    tx[1] = func;
    tx[2] = addr >> 8;
    tx[3] = addr & 0xFF;
    tx[4] = qty >> 8;
    tx[5] = qty & 0xFF;

    esp_err_t ok = mb_txrx(tx, 6, rx, &rlen, sizeof(rx), timeout);
    if (ok != ESP_OK) return ok;

    if (rx[0] != id) return ESP_ERR_INVALID_RESPONSE;
    if (rx[1] != func) return ESP_ERR_INVALID_RESPONSE;

    int bc = rx[2];
    for (int i = 0; i < qty; i++)
        out[i] = (rx[3 + 2 * i] << 8) | rx[4 + 2 * i];

    return ESP_OK;
}

esp_err_t modbus_rtu_read_holding(uint8_t id, uint16_t addr,
                                  uint16_t qty, uint16_t *out, uint32_t t)
{
    return mb_read_common(id, 3, addr, qty, out, t);
}

esp_err_t modbus_rtu_read_input(uint8_t id, uint16_t addr,
                                uint16_t qty, uint16_t *out, uint32_t t)
{
    return mb_read_common(id, 4, addr, qty, out, t);
}

esp_err_t modbus_rtu_write_single(uint8_t id, uint16_t addr,
                                  uint16_t value, uint32_t timeout)
{
    uint8_t tx[16], rx[32];
    int rlen;

    tx[0] = id;
    tx[1] = 0x06;
    tx[2] = addr >> 8;
    tx[3] = addr & 0xFF;
    tx[4] = value >> 8;
    tx[5] = value & 0xFF;

    esp_err_t ok = mb_txrx(tx, 6, rx, &rlen, sizeof(rx), timeout);
    if (ok != ESP_OK) return ok;

    if (memcmp(tx, rx, 6) != 0) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static void poll_task(void *arg)
{
    uint16_t regs[32];

    while (1) {
        ESP_LOGI(TAG, "Task RS-485");
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

    modbus_rtu_init();

    xTaskCreate(poll_task, "poll", 4096, NULL, 5, NULL);
}