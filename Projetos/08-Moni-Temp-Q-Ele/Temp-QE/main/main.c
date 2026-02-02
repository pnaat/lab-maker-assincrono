#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_log.h"

static const char *TAG = "NTC10K";

// === Parâmetros do sensor/divisor ===
#define R_FIXED_OHMS      10000.0f      // 10k
#define R0_OHMS           10000.0f      // 10k @ 25°C
#define T0_K              298.15f       // 25°C em Kelvin
#define BETA_K            3950.0f       // ajuste p/ seu NTC: 3435, 3950, etc.
#define SAMPLES           64            // multisampling

// === ADC config (ESP32 ADC1 canal 6 = GPIO34) ===
#define ADC_CHANNEL       ADC1_CHANNEL_6
#define ADC_ATTEN         ADC_ATTEN_DB_11
#define ADC_WIDTH         ADC_WIDTH_BIT_12

static esp_adc_cal_characteristics_t adc_chars;

static uint32_t read_adc_mv(void)
{
    uint32_t adc_reading = 0;
    for (int i = 0; i < SAMPLES; i++) {
        adc_reading += adc1_get_raw(ADC_CHANNEL);
    }
    adc_reading /= SAMPLES;

    uint32_t voltage_mv = 0;
    voltage_mv = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
    return voltage_mv; // em mV
}

void app_main(void)
{
    // 1) Configura ADC
    adc1_config_width(ADC_WIDTH);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);

    // 2) Caracteriza ADC com eFuse/curva
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH, 1100, &adc_chars);
    // A Vref "1100" é só default; a correta vem do eFuse se disponível.
    switch (val_type) {
        case ESP_ADC_CAL_VAL_EFUSE_TP:
            ESP_LOGI(TAG, "Calib: Two Point (eFuse)");
            break;
        case ESP_ADC_CAL_VAL_EFUSE_VREF:
            ESP_LOGI(TAG, "Calib: eFuse Vref");
            break;
        default:
            ESP_LOGW(TAG, "Calib: Default Vref (1100 mV) – menos preciso");
            break;
    }

    // 3) Loop de leitura
    while (1) {
        // 3.1) Lê tensão no nó do divisor
        uint32_t v_adc_mv = read_adc_mv();        // mV no pino ADC
        float Vadc = v_adc_mv / 1000.0f;          // em Volts

        // 3.2) Tensão de referência do divisor (topo = 3V3 real)
        // Melhor: medir 3V3 com outro canal e usar esse valor.
        const float Vref = 3.30f;                 // ajuste p/ seu board

        // Proteção contra limites
        if (Vadc < 0.001f) Vadc = 0.001f;
        if (Vadc > Vref - 0.001f) Vadc = Vref - 0.001f;

        // 3.3) Converte para Rntc
        float Rntc = R_FIXED_OHMS * (Vadc / (Vref - Vadc));

        // 3.4) Beta equation -> Temperatura (Kelvin)
        float invT = (1.0f / T0_K) + (1.0f / BETA_K) * logf(Rntc / R0_OHMS);
        float T_k = 1.0f / invT;
        float T_c = T_k - 273.15f;

        ESP_LOGI(TAG, "Vadc=%.3f V, Rntc=%.1f ohms, T=%.2f °C", Vadc, Rntc, T_c);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}