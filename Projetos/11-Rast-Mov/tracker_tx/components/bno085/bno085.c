#include "bno085.h"
#include <string.h>
#include <math.h>
#include "driver/i2c.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "BNO085";

// ---- HARDWARE I2C (avoid OLED bus) ----
// Heltec V3 typical OLED uses GPIO17/18/21; we'll use second I2C on 42/41.
#define I2C_PORT    I2C_NUM_0
#define PIN_SDA     42
#define PIN_SCL     41
#define I2C_FREQ    400000
#define BNO_ADDR    0x4B  // common addr for GY-BNO085 when ADR=HIGH; change to 0x4A if needed

// ---- SHTP constants ----
#define SHTP_HEADER_LEN 4
#define SHTP_MAX_PAYLOAD 200
#define CHAN_CONTROL 0
#define CHAN_EXECUTABLE 1
#define CHAN_REPORTS 2

// SH-2 sensor IDs
#define SENS_ACCEL_UNCAL 0x05

// Feature command (set feature)
#define SHTP_FEAT_CMD_SET_FEATURE 0xFD

static uint8_t rxbuf[256];
static float s_ax=0, s_ay=0, s_az=0; static bool s_have=false;

static inline uint64_t ms(){ return esp_timer_get_time()/1000ULL; }

static esp_err_t i2c_write(const uint8_t *data, size_t len){
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BNO_ADDR<<1)|0, true);
    i2c_master_write(cmd, (uint8_t*)data, len, true);
    i2c_master_stop(cmd);
    esp_err_t e = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return e;
}

static esp_err_t i2c_read(uint8_t *data, size_t len){
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BNO_ADDR<<1)|1, true);
    i2c_master_read(cmd, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t e = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return e;
}

static bool shtp_read_packet(uint8_t *chan, uint8_t **payload, uint16_t *plen){
    // Read 4-byte header first
    if (i2c_read(rxbuf, 4) != ESP_OK) return false;
    uint16_t len = rxbuf[0] | (rxbuf[1]<<8); // includes header
    if (!len || len < 4 || len > sizeof(rxbuf)) return false;
    uint16_t remain = len - 4;
    if (remain){ if (i2c_read(rxbuf+4, remain) != ESP_OK) return false; }
    *chan = rxbuf[2];
    *payload = rxbuf+4;
    *plen = remain;
    return true;
}

static void shtp_send_command(uint8_t channel, const uint8_t *payload, uint16_t plen){
    // Build header (len includes header)
    uint8_t hdr[4] = { (uint8_t)((plen+4)&0xFF), (uint8_t)(((plen+4)>>8)&0xFF), channel, 0x00 };
    // Write header + payload in one I2C transaction if possible
    uint8_t buf[4+SHTP_MAX_PAYLOAD];
    memcpy(buf, hdr, 4);
    if (plen) memcpy(buf+4, payload, plen);
    i2c_write(buf, 4+plen);
}

static void enable_accel_uncal(uint16_t period_us){
    // SH-2 Set Feature command payload: [FeatureCmd=0xFD, SensorId, ReportInterval(4B), Batch(4B), SensorSpecific(4B)]
    uint8_t p[1+1+4+4+4];
    memset(p, 0, sizeof(p));
    p[0] = SHTP_FEAT_CMD_SET_FEATURE;
    p[1] = SENS_ACCEL_UNCAL;
    // Report interval in microseconds (little-endian)
    uint32_t us = period_us;
    p[2]=us&0xFF; p[3]=(us>>8)&0xFF; p[4]=(us>>16)&0xFF; p[5]=(us>>24)&0xFF;
    // Batch=0; SensorSpecific=0
    shtp_send_command(CHAN_CONTROL, p, sizeof(p));
}

esp_err_t bno085_init(void){
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    // Dummy read to clear any pending packet
    uint8_t ch; uint8_t *pl; uint16_t ln;
    for (int i=0;i<3;i++){ shtp_read_packet(&ch, &pl, &ln); }

    // Enable uncalibrated accelerometer at ~40 Hz (25,000 us)
    enable_accel_uncal(25000);
    return ESP_OK;
}

bool bno085_get_accel_g(float *ax_g, float *ay_g, float *az_g){
    uint8_t ch; uint8_t *pl; uint16_t ln; bool got=false;
    // Poll a few packets quickly
    for (int i=0;i<4;i++){
        if (!shtp_read_packet(&ch, &pl, &ln)) break;
        if (ch == CHAN_REPORTS && ln >= 1+6){
            uint8_t reportId = pl[0];
            // For simplicity, many firmwares map accel reportId=0x05/0x07; we check length 1+6 (3x int16)
            int16_t x = (int16_t)(pl[1] | (pl[2]<<8));
            int16_t y = (int16_t)(pl[3] | (pl[4]<<8));
            int16_t z = (int16_t)(pl[5] | (pl[6]<<8));
            // Scale: assume Q format 1/2048 g per LSB (approx for some BNO configs)
            float ax = x / 2048.0f; float ay = y / 2048.0f; float az = z / 2048.0f;
            s_ax=ax; s_ay=ay; s_az=az; s_have=true; got=true;
        }
    }
    if (s_have && ax_g && ay_g && az_g){ *ax_g=s_ax; *ay_g=s_ay; *az_g=s_az; return true; }
    return got;
}

bool bno085_has_motion(void){
    static uint64_t t_above=0, t_below=0; static bool state=false; uint64_t now=ms();
    float ax=0, ay=0, az=0; bool ok=bno085_get_accel_g(&ax,&ay,&az);
    if (!ok) return state; // keep last
    float am = sqrtf(ax*ax+ay*ay+az*az);
    const float TH_HIGH=0.20f, TH_LOW=0.10f; // g
    const uint32_t T_ABOVE=1000, T_BELOW=3000; // ms
    if (am>TH_HIGH){ if(!t_above) t_above=now; t_below=0; }
    else if (am<TH_LOW){ if(!t_below) t_below=now; t_above=0; }
    bool motion=(t_above && (now - t_above >= T_ABOVE));
    bool still =(t_below && (now - t_below >= T_BELOW));
    if (motion){
        state=true;
    }  
    if (still){
        state=false;
    }
    return state;
}
