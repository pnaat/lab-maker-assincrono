#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "ssd1306.h"

// Headers específicos da nova versão do RC522
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"

static const char *TAG = "SISTEMA_MANUTENCAO";

// --- CONFIGURAÇÕES ---
#define WIFI_SSID       "NOME_DA_REDE"
#define WIFI_PASS       "SENHA_DA_REDE"
#define WEB_DEPLOY_URL  "https://script.google.com/macros/s/SEU_ID/exec"

// Pinagem Heltec V3
#define OLED_RST          21
#define VEXT_CTRL         18 
#define RFID_MISO         11
#define RFID_MOSI         10
#define RFID_SCK          9
#define RFID_SDA          8

// Globais
ssd1306_handle_t oled = NULL;
static rc522_driver_handle_t rfid_driver;
static rc522_handle_t rfid_scanner;

// --- FUNÇÃO DE ENVIO HTTP ---
void enviar_dados_planilha(const char* uid) {
    char url_final[512];
    snprintf(url_final, sizeof(url_final), "%s?ID=%s", WEB_DEPLOY_URL, uid);

    esp_http_client_config_t config = {
        .url = url_final,
        .method = HTTP_METHOD_GET,
        .disable_auto_redirect = false,
        .is_async = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Dados enviados! Status: %d", esp_http_client_get_status_code(client));
        ssd1306_draw_string(oled, 0, 45, (const uint8_t*)"ENVIADO OK", 12, 1);
    } else {
        ESP_LOGE(TAG, "Erro no envio: %s", esp_err_to_name(err));
        ssd1306_draw_string(oled, 0, 45, (const uint8_t*)"ERRO ENVIO", 12, 1);
    }
    ssd1306_refresh_gram(oled);
    esp_http_client_cleanup(client);
}

// --- HANDLER RFID (NOVO MODELO v4.0) ---
static void on_rfid_event(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        char uid_str[20];
        snprintf(uid_str, sizeof(uid_str), "%02X%02X%02X%02X", 
                 picc->uid.value[0], picc->uid.value[1], 
                 picc->uid.value[2], picc->uid.value[3]);

        ESP_LOGI(TAG, "Tag detectada: %s", uid_str);

        // Atualiza Display
        ssd1306_clear_screen(oled, 0x00);
        ssd1306_draw_string(oled, 0, 0, (const uint8_t*)"TAG IDENTIFICADA", 12, 1);
        ssd1306_draw_string(oled, 0, 20, (const uint8_t*)uid_str, 12, 1);
        ssd1306_draw_string(oled, 0, 45, (const uint8_t*)"ENVIANDO...", 12, 1);
        ssd1306_refresh_gram(oled);

        enviar_dados_planilha(uid_str);
    }
}

// --- SETUP INICIAL ---
void app_main(void) {
    // 1. NVS e Event Loop
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_event_loop_create_default();

    // 2. Wi-Fi (Simplificado)
    // [Aqui deve ir sua função de conexão Wi-Fi padrão]

    // 3. Inicializar Driver RC522 (Novo Modelo)
    rc522_spi_config_t spi_config = {
        .host_id = SPI2_HOST,
        .bus_config = &(spi_bus_config_t){
            .miso_io_num = RFID_MISO,
            .mosi_io_num = RFID_MOSI,
            .sclk_io_num = RFID_SCK,
        },
        .dev_config = {
            .spics_io_num = RFID_SDA,
        },
        .rst_io_num = -1,
    };

    rc522_spi_create(&spi_config, &rfid_driver);
    rc522_driver_install(rfid_driver);

    // 4. Inicializar Scanner
    rc522_config_t scanner_config = {
        .driver = rfid_driver,
    };

    rc522_create(&scanner_config, &rfid_scanner);
    rc522_register_events(rfid_scanner, RC522_EVENT_PICC_STATE_CHANGED, on_rfid_event, NULL);
    rc522_start(rfid_scanner);

    ESP_LOGI(TAG, "Sistema de Manutencao Iniciado");
}