#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lora.h"

static const char *TAG = "GATEWAY_RX";

typedef struct __attribute__((packed)){
    int32_t lat;
    int32_t lon;
    int16_t spd;
    uint8_t flags;
    uint8_t sats;
} payload_t;

void app_main(void){
    ESP_ERROR_CHECK(lora_init());
    ESP_ERROR_CHECK(lora_set_rx_continuous());

    uint8_t buf[32];
    while(1){
        int rssi=0, snr=0;
        int n = lora_receive_packet(buf, sizeof(buf), 5000, &rssi, &snr);
        if (n == sizeof(payload_t)){
            payload_t *p=(payload_t*)buf;
            float lat = p->lat/1e6f; float lon = p->lon/1e6f; float v=p->spd/10.0f;
            int motion = (p->flags & 0x02) ? 1 : 0;
            ESP_LOGI(TAG, "RX: lat=%.6f lon=%.6f v=%.1f km/h sats=%u fix=%d motion=%d", lat,lon,v,p->sats,(p->flags&1)!=0,motion);
        }
    }
}
