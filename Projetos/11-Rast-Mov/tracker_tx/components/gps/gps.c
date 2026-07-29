#include "gps.h"
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define GPS_UART_NUM   UART_NUM_1
#define GPS_TX_PIN     43
#define GPS_RX_PIN     44
#define GPS_BAUD       9600

static gps_fix_t s_fix;
static SemaphoreHandle_t s_mux;

static double nmea_deg(const char *s){
    if(!s||!*s) return 0.0;
    double v = atof(s);
    int d = (int)(v/100.0);
    double m = v - d*100.0;
    return d + m/60.0;
}

static void parse_rmc(char **f, int nf){
    if (nf < 8) return;
    if (f[2][0] != 'A'){ s_fix.valid = false; return; }
    double lat = nmea_deg(f[3]); if (f[4][0]=='S') lat = -lat;
    double lon = nmea_deg(f[5]); if (f[6][0]=='W') lon = -lon;
    float sp  = f[7] ? (float)(atof(f[7]) * 1.852) : 0.0f;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_fix.lat = lat; s_fix.lon = lon; s_fix.speed_kmh = sp; s_fix.valid = true;
    xSemaphoreGive(s_mux);
}

static void parse_gga(char **f, int nf){
    if (nf < 8) return;
    int sats = f[7] ? atoi(f[7]) : 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_fix.sats = sats;
    xSemaphoreGive(s_mux);
}

static void gps_task(void *arg){
    uint8_t rx[256];
    char line[256]; int lp=0;
    while(1){
        int n = uart_read_bytes(GPS_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(50));
        for(int i=0;i<n;i++){
            char c = (char)rx[i];
            if (c=='\r') continue;
            if (c=='\n'){
                line[lp]=0; lp=0;
                if (line[0]=='$'){
                    char *p=line; char *f[32]; int nf=0;
                    while((f[nf]=strsep(&p, ",")) && nf<31) nf++;
                    if (strstr(line, "RMC,")) parse_rmc(f, nf);
                    else if (strstr(line, "GGA,")) parse_gga(f, nf);
                }
            } else if (lp < (int)sizeof(line)-1) {
                line[lp++] = c;
            } else {
                lp = 0; // overflow, discard
            }
        }
    }
}

esp_err_t gps_init(void){
    s_mux = xSemaphoreCreateMutex();
    memset(&s_fix, 0, sizeof(s_fix));

    uart_config_t cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    uart_driver_install(GPS_UART_NUM, 2048, 0, 0, NULL, 0);
    uart_param_config(GPS_UART_NUM, &cfg);
    uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreatePinnedToCore(gps_task, "gps_task", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    return ESP_OK;
}

bool gps_get_fix(gps_fix_t *out){
    if (!out) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_fix;
    xSemaphoreGive(s_mux);
    return s_fix.valid;
}
