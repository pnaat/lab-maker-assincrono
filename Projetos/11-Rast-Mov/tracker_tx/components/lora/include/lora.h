#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Hardcoded SX1262 P2P driver for Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
// Pins (per Heltec V3 schematics/community refs):
//  NSS/CS=GPIO8, SCK=GPIO9, MOSI=GPIO10, MISO=GPIO11, RST=GPIO12, BUSY=GPIO13, DIO1=GPIO14
// Radio params: Freq=915 MHz, BW=125 kHz, SF7, CR=4/5, TX=17 dBm

esp_err_t lora_init(void);
esp_err_t lora_set_rx_continuous(void);
int       lora_receive_packet(uint8_t *buf, int maxlen, int timeout_ms, int *rssi_dbm, int *snr_db);
esp_err_t lora_send(const uint8_t *data, uint8_t len, uint32_t timeout_ms);
