#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    double lat;       // degrees
    double lon;       // degrees
    float  speed_kmh; // km/h
    int    sats;      // satellites (from GGA if parsed)
    bool   valid;     // true if fix is valid (RMC 'A')
} gps_fix_t;

esp_err_t gps_init(void);     // start UART + parser task
bool      gps_get_fix(gps_fix_t *out);
