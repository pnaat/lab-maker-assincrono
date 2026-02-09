#pragma once
#include <stdbool.h>
#include "esp_err.h"

// BNO085 minimal SHTP client: enables Uncalibrated Accelerometer and exposes motion detection with hysteresis.

esp_err_t bno085_init(void);            // I2C + SHTP init + enable accel
bool      bno085_get_accel_g(float *ax_g, float *ay_g, float *az_g); // returns latest accel in g
bool      bno085_has_motion(void);      // hysteresis-based motion flag
