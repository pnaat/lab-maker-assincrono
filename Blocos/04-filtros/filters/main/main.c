#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h" // Apenas para gerar dados de teste

// --- Configurações dos Filtros ---
#define SMA_WINDOW_SIZE 10    // Tamanho da janela do SMA
#define EMA_ALPHA 0.2f        // Fator de suavização do EMA (0.0 a 1.0)

// Variáveis Globais para os Filtros
float sma_buffer[SMA_WINDOW_SIZE] = {0};
int sma_index = 0;
float last_ema_value = 0;

/**
 * Filtro de Média Móvel Simples (SMA)
 * Soma os últimos N valores e divide por N.
 */
float apply_sma(float input) {
    sma_buffer[sma_index] = input; // Substitui o valor mais antigo
    sma_index = (sma_index + 1) % SMA_WINDOW_SIZE; // Move o índice circularmente

    float sum = 0;
    for (int i = 0; i < SMA_WINDOW_SIZE; i++) {
        sum += sma_buffer[i];
    }
    return sum / SMA_WINDOW_SIZE;
}

/**
 * Filtro de Média Móvel Exponencial (EMA)
 * Fórmula: Out = (Alpha * In) + ((1 - Alpha) * Out_anterior)
 */
float apply_ema(float input) {
    last_ema_value = (EMA_ALPHA * input) + ((1.0f - EMA_ALPHA) * last_ema_value);
    return last_ema_value;
}

void app_main(void) {
    printf("Iniciando Filtros SMA vs EMA\n");

    while (1) {
        // Simulando um sinal de 25.0 com ruído aleatório
        float noisy_signal = 25.0f + ((float)(esp_random() % 100) / 20.0f);

        float filtered_sma = apply_sma(noisy_signal);
        float filtered_ema = apply_ema(noisy_signal);

        printf("Original: %.2f | SMA: %.2f | EMA: %.2f\n", 
                noisy_signal, filtered_sma, filtered_ema);

        vTaskDelay(pdMS_TO_TICKS(500)); // Espera 500ms
    }
}