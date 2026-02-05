#include "modbus_rtu.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UART_PORT      UART_NUM_1
#define UART_TX_PIN     17
#define UART_RX_PIN     18
#define UART_RTS_PIN    16
#define UART_BAUD    19200
#define UART_PARITY UART_PARITY_EVEN
#define UART_STOP    UART_STOP_BITS_1

static const char *TAG = "modbus_rtu";

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
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY,
        .stop_bits = UART_STOP,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_RTS_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(UART_PORT, UART_MODE_RS485_HALF_DUPLEX));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 0, 0, NULL, 0));

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