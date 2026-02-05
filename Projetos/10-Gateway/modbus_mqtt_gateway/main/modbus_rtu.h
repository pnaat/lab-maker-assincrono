#pragma once
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"

esp_err_t modbus_rtu_init(void);

esp_err_t modbus_rtu_read_holding(uint8_t slave, uint16_t addr,
                                  uint16_t qty, uint16_t *out, uint32_t timeout);

esp_err_t modbus_rtu_read_input(uint8_t slave, uint16_t addr,
                                uint16_t qty, uint16_t *out, uint32_t timeout);

esp_err_t modbus_rtu_write_single(uint8_t slave, uint16_t addr,
                                  uint16_t value, uint32_t timeout);