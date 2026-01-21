#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

// Inclusões NimBLE (Stack BLE)
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "SENSOR_TANQUE_PRO";

// --- CONFIGURAÇÕES ---
#define POT_ADC_CHANNEL    ADC_CHANNEL_0 // GPIO 1
#define LED_ALERTA_GPIO    2
#define INTERVALO_MEDICAO  10000         // 10 segundos para teste (600000 para 10min)
#define NVS_KEY_CONSUMO    "cons_medio"

// Variáveis Globais
float nivel_atual = 0;
float consumo_medio_hora = 0;
float ultimo_nivel = -1;
uint8_t alerta_vazamento = 0;

// UUIDs BLE
static const ble_uuid128_t SENSOR_SVC_UUID = {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0xa4, 0x34, 0x06, 0x07, 0x11, 0xbb, 0x20, 0x8d, 0x24, 0x46, 0xba, 0x73, 0x20, 0xa1, 0x71, 0x2d}
};

static const ble_uuid128_t NIVEL_CHR_UUID = {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0xa5, 0x34, 0x06, 0x07, 0x11, 0xbb, 0x20, 0x8d, 0x24, 0x46, 0xba, 0x73, 0x20, 0xa1, 0x71, 0x2d}
};
uint16_t nivel_handle;

// --- FUNÇÕES DE MEMÓRIA (NVS) ---

void salvar_consumo_nvs(float valor) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_blob(my_handle, NVS_KEY_CONSUMO, &valor, sizeof(float));
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

void carregar_consumo_nvs() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t size = sizeof(float);
        nvs_get_blob(my_handle, NVS_KEY_CONSUMO, &consumo_medio_hora, &size);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Consumo médio recuperado da NVS: %.4f", consumo_medio_hora);
    }
}

// --- LÓGICA DE PROCESSAMENTO ---

void executar_logica_tanque(int adc_raw) {
    nivel_atual = (adc_raw * 100.0) / 4095.0;

    if (ultimo_nivel != -1) {
        float consumo_detectado = ultimo_nivel - nivel_atual;

        // Se o consumo for positivo (nível descendo)
        if (consumo_detectado > 0.05) { // Filtro de pequena oscilação
            
            // VERIFICAÇÃO DE ALERTA (Ex: 20% acima da média)
            if (consumo_medio_hora > 0 && consumo_detectado > (consumo_medio_hora * 1.20)) {
                alerta_vazamento = 1;
                gpio_set_level(LED_ALERTA_GPIO, 1);
                ESP_LOGW(TAG, "!!! ALERTA DE CONSUMO EXCESSIVO !!!");
            } else {
                alerta_vazamento = 0;
                gpio_set_level(LED_ALERTA_GPIO, 0);
            }

            // Atualiza média móvel e salva
            consumo_medio_hora = (consumo_medio_hora == 0) ? consumo_detectado : (consumo_medio_hora * 0.9 + consumo_detectado * 0.1);
            salvar_consumo_nvs(consumo_medio_hora);
        }
    }
    ultimo_nivel = nivel_atual;
    ESP_LOGI(TAG, "Nível: %.1f%% | Consumo Médio: %.4f", nivel_atual, consumo_medio_hora);
}

// --- INTEGRAÇÃO BLE ---

static int gatt_svr_chr_access_nivel(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    char info[50];
    snprintf(info, sizeof(info), "N:%.1f%%|C:%.2f|A:%d", nivel_atual, consumo_medio_hora, alerta_vazamento);
    os_mbuf_append(ctxt->om, info, strlen(info));
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &SENSOR_SVC_UUID.u, // Acessando o membro .u (base do UUID)
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &NIVEL_CHR_UUID.u,
          .access_cb = gatt_svr_chr_access_nivel,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
          .val_handle = &nivel_handle},
         {0}}},
    {0}};

// --- CORE TASKS ---

void sensor_task(void *pvParameters) {
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&init_config, &adc1_handle);
    adc_oneshot_chan_cfg_t config = {.bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12};
    adc_oneshot_config_channel(adc1_handle, POT_ADC_CHANNEL, &config);

    carregar_consumo_nvs();

    while (1) {
        int raw;
        adc_oneshot_read(adc1_handle, POT_ADC_CHANNEL, &raw);
        executar_logica_tanque(raw);
        
        // Notifica o app conectado que o valor mudou
        ble_gatts_chr_updated(nivel_handle);
        
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_MEDICAO));
    }
}

// Boilerplate NimBLE (Necessário para o funcionamento do rádio)
void on_sync(void) {
    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);
    struct ble_gap_adv_params adv_params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    struct ble_hs_adv_fields fields = {0};
    fields.name = (uint8_t *)"Tanque_Smart_S3";
    fields.name_len = 15;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
}

void host_task(void *param) { nimble_port_run(); }

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_reset_pin(LED_ALERTA_GPIO);
    gpio_set_direction(LED_ALERTA_GPIO, GPIO_MODE_OUTPUT);

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}