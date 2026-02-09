#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lora.h"
#include "gps.h"
#include "bno085.h"

static const char *TAG = "TRACKER_TX";

typedef struct __attribute__((packed)){
    int32_t lat;   // deg * 1e6
    int32_t lon;   // deg * 1e6
    int16_t spd;   // km/h * 10
    uint8_t flags; // bit0=fix, bit1=motion
    uint8_t sats;  // satellites
} payload_t;

void app_main(void){
    ESP_ERROR_CHECK(lora_init());
    ESP_ERROR_CHECK(gps_init());
    ESP_ERROR_CHECK(bno085_init());

    while(1){
        bool motion = bno085_has_motion();
        if (!motion){ vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        gps_fix_t f={0};
        gps_get_fix(&f);
        if (!f.valid){ vTaskDelay(pdMS_TO_TICKS(500)); continue; }

        payload_t p = {
            .lat  = (int32_t)(f.lat * 1e6),
            .lon  = (int32_t)(f.lon * 1e6),
            .spd  = (int16_t)(f.speed_kmh * 10),
            .flags= (uint8_t)((f.valid?1:0) | (motion?2:0)),
            .sats = (uint8_t)(f.sats & 0xFF)
        };
        ESP_LOGI(TAG, "TX -> lat=%.6f lon=%.6f v=%.1f km/h sats=%d (motion=%d)", f.lat,f.lon,f.speed_kmh,f.sats,motion);
        lora_send((uint8_t*)&p, sizeof(p), 3000);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
