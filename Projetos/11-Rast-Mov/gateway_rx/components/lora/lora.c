#include "lora.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "LORA";

// -------------------- HARDWARE PINS (Heltec WiFi LoRa 32 V3) --------------------
#define LORA_SPI_HOST   SPI2_HOST
#define PIN_SCLK        9
#define PIN_MISO        11
#define PIN_MOSI        10
#define PIN_NSS         8
#define PIN_RST         12
#define PIN_BUSY        13
#define PIN_DIO1        14

// -------------------- RADIO PARAMS (hardcoded P2P) ----------------------------
#define LORA_FREQ_HZ    915000000UL
#define LORA_BW_CODE    0x07  // 125 kHz
#define LORA_SF_CODE    7     // SF7
#define LORA_CR_CODE    1     // 4/5 -> code=1
#define LORA_TX_DBM     17
#define LORA_PREAMBLE   8

static spi_device_handle_t s_spi;

static inline void cs_low(void){ gpio_set_level(PIN_NSS, 0); }
static inline void cs_high(void){ gpio_set_level(PIN_NSS, 1); }
static inline uint64_t millis(void){ return esp_timer_get_time()/1000ULL; }
static inline void wait_busy(void){ while(gpio_get_level(PIN_BUSY)){} }

static void spi_tx(const uint8_t *data, int len){
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static void spi_rx(uint8_t *data, int len){
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.rxlength = len * 8;
    t.rx_buffer = data;
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static void write_cmd(uint8_t cmd, const uint8_t *buf, int len){
    wait_busy();
    cs_low();
    spi_tx(&cmd, 1);
    if (len && buf) spi_tx(buf, len);
    cs_high();
}

static void read_cmd(uint8_t cmd, uint8_t *buf, int len){
    wait_busy();
    cs_low();
    spi_tx(&cmd, 1);
    uint8_t dummy = 0x00; // one dummy byte per datasheet
    spi_tx(&dummy, 1);
    if (len && buf) spi_rx(buf, len);
    cs_high();
}

static void write_buffer(uint8_t offset, const uint8_t *buf, int len){
    wait_busy();
    cs_low();
    uint8_t hdr = 0x0E; // WRITE_BUFFER
    spi_tx(&hdr, 1);
    spi_tx(&offset, 1);
    spi_tx(buf, len);
    cs_high();
}

static void read_buffer(uint8_t offset, uint8_t *buf, int len){
    wait_busy();
    cs_low();
    uint8_t hdr = 0x1E; // READ_BUFFER
    uint8_t dummy = 0x00;
    spi_tx(&hdr, 1);
    spi_tx(&offset, 1);
    spi_tx(&dummy, 1);
    spi_rx(buf, len);
    cs_high();
}

static uint32_t freq_to_reg(uint32_t hz){
    const double step = 32e6 / (double)(1UL<<25); // ~0.953674316 Hz
    return (uint32_t)(hz / step);
}

static void set_standby_rc(void){
    uint8_t p = 0x00; // RC
    write_cmd(0x80, &p, 1); // SET_STANDBY
}

static void set_packet_type_lora(void){
    uint8_t p = 0x01; // LoRa
    write_cmd(0x8A, &p, 1);
}

static void set_rf_frequency(uint32_t hz){
    uint32_t fr = freq_to_reg(hz);
    uint8_t p[4] = { (uint8_t)(fr>>24), (uint8_t)(fr>>16), (uint8_t)(fr>>8), (uint8_t)fr };
    write_cmd(0x86, p, 4);
}

static void set_buffer_bases(uint8_t tx_base, uint8_t rx_base){
    uint8_t p[2] = { tx_base, rx_base };
    write_cmd(0x8F, p, 2);
}

static void set_mod_params(uint8_t sf_code, uint8_t bw_code, uint8_t cr_code, bool ldro){
    uint8_t p[4] = { sf_code, bw_code, cr_code, (uint8_t)(ldro ? 1 : 0) };
    write_cmd(0x8B, p, 4);
}

static void set_pkt_params(uint16_t preamble, bool implicit, uint8_t payload_len, bool crc_on, bool iq_inv){
    uint8_t p[6] = {
        (uint8_t)(preamble>>8), (uint8_t)preamble,
        (uint8_t)(implicit ? 1 : 0),
        payload_len,
        (uint8_t)(crc_on ? 1 : 0),
        (uint8_t)(iq_inv ? 1 : 0)
    };
    write_cmd(0x8C, p, 6);
}

static void set_tx_params(int8_t power_dbm){
    uint8_t p[2] = { (uint8_t)power_dbm, 0x04 /* 200us */ };
    write_cmd(0x8E, p, 2);
}

static void clear_irq(uint16_t mask){
    uint8_t p[2] = { (uint8_t)(mask>>8), (uint8_t)mask };
    write_cmd(0x02, p, 2);
}

static uint16_t get_irq(void){
    uint8_t r[2];
    read_cmd(0x12, r, 2);
    return ((uint16_t)r[0]<<8) | r[1];
}

static void set_dio_irq(uint16_t mask, uint16_t dio1_mask){
    uint8_t p[8] = {
        (uint8_t)(mask>>8), (uint8_t)mask,
        0x00, 0x00,
        (uint8_t)(dio1_mask>>8), (uint8_t)dio1_mask,
        0x00, 0x00
    };
    write_cmd(0x08, p, 8);
}

static void set_tx(uint32_t timeout_ms){
    uint32_t ticks = (timeout_ms * 1000) / 16; // ~15.625us
    uint8_t p[3] = { (uint8_t)(ticks>>16), (uint8_t)(ticks>>8), (uint8_t)ticks };
    write_cmd(0x83, p, 3);
}

static void set_rx_cont(void){
    uint8_t p[3] = { 0xFF, 0xFF, 0xFF };
    write_cmd(0x82, p, 3);
}

static void get_rx_buffer_status(uint8_t *payload_len, uint8_t *rx_start){
    uint8_t r[2];
    read_cmd(0x13, r, 2);
    if (payload_len) *payload_len = r[0];
    if (rx_start) *rx_start = r[1];
}

esp_err_t lora_init(void){
    // GPIOs
    gpio_config_t io = {0};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL<<PIN_NSS) | (1ULL<<PIN_RST);
    gpio_config(&io);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = 1;
    io.pin_bit_mask = (1ULL<<PIN_BUSY) | (1ULL<<PIN_DIO1);
    gpio_config(&io);

    // SPI
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LORA_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 8*1000*1000,
        .mode = 0,
        .spics_io_num = -1, // manual CS
        .queue_size = 3
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LORA_SPI_HOST, &dev, &s_spi));

    // Reset
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Basic config
    set_standby_rc();
    set_packet_type_lora();
    set_buffer_bases(0x00, 0x00);
    set_rf_frequency(LORA_FREQ_HZ);

    // Mod params (LDRO heuristic)
    bool ldro = (LORA_BW_CODE <= 0x06 /* <=62.5k */ && LORA_SF_CODE >= 11);
    set_mod_params(LORA_SF_CODE, LORA_BW_CODE, LORA_CR_CODE, ldro);

    // Packet params explicit header, variable length, CRC on, IQ normal
    set_pkt_params(LORA_PREAMBLE, false, 0xFF, true, false);
    set_tx_params(LORA_TX_DBM);

    clear_irq(0xFFFF);
    set_dio_irq(0xFFFF, 0xFFFF); // map all to DIO1

    ESP_LOGI(TAG, "SX1262 ready (Heltec V3 hardcoded pins)");
    return ESP_OK;
}

esp_err_t lora_set_rx_continuous(void){
    clear_irq(0xFFFF);
    set_rx_cont();
    return ESP_OK;
}

int lora_receive_packet(uint8_t *buf, int maxlen, int timeout_ms, int *rssi_dbm, int *snr_db){
    uint64_t start = millis();
    while ((millis() - start) < (uint64_t)timeout_ms){
        uint16_t irq = get_irq();
        if (irq & 0x0002 /* RX_DONE */){
            clear_irq(irq);
            uint8_t len=0, off=0;
            get_rx_buffer_status(&len, &off);
            if (len > maxlen) len = maxlen;
            read_buffer(off, buf, len);
            // (optional) could read packet status 0x14 for RSSI/SNR
            if (rssi_dbm) *rssi_dbm = 0;
            if (snr_db)   *snr_db   = 0;
            return (int)len;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

esp_err_t lora_send(const uint8_t *data, uint8_t len, uint32_t timeout_ms){
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    clear_irq(0xFFFF);
    write_buffer(0x00, data, len);
    set_tx(timeout_ms);

    uint64_t start = millis();
    while ((millis() - start) < (timeout_ms + 100)){
        uint16_t irq = get_irq();
        if (irq & 0x0001 /* TX_DONE */){
            clear_irq(irq);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}
