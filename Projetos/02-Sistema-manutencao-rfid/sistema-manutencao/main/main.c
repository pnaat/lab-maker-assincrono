#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "ssd1306.h"
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "SISTEMA_MANUTENCAO";

// --- CONFIGURAÇÕES ---
#define WIFI_SSID       "XXXXXXX"
#define WIFI_PASS       "XXXXXXX"
#define WEB_DEPLOY_URL  "https://script.google.com/macros/s/XXXXXXXXXXX/exec"
#define WIFI_MAX_RETRY   5

// Pinagem Heltec V3
#define I2C_MASTER_NUM    0
#define OLED_SDA          17
#define OLED_SCL          18
#define OLED_RST          21
#define VEXT_CTRL         36 

#define RFID_MISO         7
#define RFID_MOSI         6
#define RFID_SCK          5
#define RFID_SDA          4

// Nome da máquina
#define MAQUINA_NOME   "PRENSA_01"   


// ===================== VARIÁVEIS GLOBAIS =======================
ssd1306_handle_t oled = NULL;
static rc522_driver_handle_t rfid_driver;
static rc522_handle_t rfid_scanner;

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

// ===================== OLED =====================

static void oled_init_heltec(void) {

    gpio_set_direction(VEXT_CTRL, GPIO_MODE_OUTPUT);
    gpio_set_level(VEXT_CTRL, 0); 
    gpio_set_direction(OLED_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(OLED_RST, 1);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = OLED_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    oled = ssd1306_create(I2C_MASTER_NUM, 0x3C);
    ssd1306_clear_screen(oled, 0x00);
    ssd1306_draw_string(oled, 0, 0, (const uint8_t*)"Iniciando...", 12, 1);
    ssd1306_refresh_gram(oled);

}


// ===================== ENVIO HTTP =====================

static int url_encode_char(char c, char *out) {
    // Retorna quantos chars foram escritos em out
    const char *hex = "0123456789ABCDEF";
    // Caracteres seguros em querystring
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
        out[0] = c;
        return 1;
    }
    out[0] = '%';
    out[1] = hex[(c >> 4) & 0xF];
    out[2] = hex[(c) & 0xF];
    return 3;
}

static void url_encode(const char *in, char *out, size_t out_len) {
    size_t oi = 0;
    for (size_t i = 0; in[i] != '\0' && oi + 4 < out_len; i++) {
        oi += url_encode_char(in[i], &out[oi]);
    }
    out[oi] = '\0';
}

void enviar_dados_planilha(const char* uid) {
    // 1) Montar timestamp local (sincronizado via SNTP)
    char ts[32];

    // 2) Encodar parâmetros
    char uid_enc[64], maq_enc[64];
    url_encode(uid, uid_enc, sizeof(uid_enc));
    url_encode(MAQUINA_NOME, maq_enc, sizeof(maq_enc));

    // 3) Montar URL final
    char url_final[512];
    // Exemplo de query: ?ID=<uid>&MAQUINA=<nome>&DATAHORA=<yyyy-mm-dd HH:MM:SS>
    snprintf(url_final, sizeof(url_final), "%s?ID=%s&MAQUINA=%s",
         WEB_DEPLOY_URL, uid_enc, maq_enc);

    // 4) HTTP GET
    esp_http_client_config_t config = {
        .url = url_final,
        .method = HTTP_METHOD_GET,
        .disable_auto_redirect = false,
        .is_async = false,
        // Opcional: timeouts para melhorar robustez
        //.timeout_ms = 8000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (status == 200 || status == 302 ) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Dados enviados! Status: %d", status);
        ssd1306_draw_string(oled, 0, 45, (const uint8_t*)"ENVIADO OK", 12, 1);
    } else {
        ESP_LOGE(TAG, "Erro no envio: %d", status);
        ssd1306_draw_string(oled, 0, 45, (const uint8_t*)"ERRO ENVIO", 12, 1);
    }
    ssd1306_refresh_gram(oled);
    esp_http_client_cleanup(client);
}

// ===================== HANDLER RFID =====================
static void on_rfid_event(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        char uid_str[32] = {0};
        int uid_len = 0;
        
        #ifdef __cplusplus
            uid_len = picc->uid.length;
        #else
            uid_len = picc->uid.length;
        #endif

        if (uid_len <= 0 || uid_len > 10) {
            // fallback defensivo: se não vier length válido, assume 4 bytes (MIFARE classic)
            uid_len = 4;
        }

        for (int i = 0; i < uid_len && i < 10; i++) {
            char b[3];
            snprintf(b, sizeof(b), "%02X", picc->uid.value[i]);
            strncat(uid_str, b, sizeof(uid_str) - strlen(uid_str) - 1);
        }

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

// ===================== WIFI =====================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    static int s_retry_num = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi desconectado, tentando reconectar... (%d)", s_retry_num);
        } else {
            ESP_LOGE(TAG, "Falha ao conectar no WiFi");
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando ao WiFi SSID:%s", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}


// --- SETUP INICIAL ---
void app_main(void) {
    // 1. NVS e Event Loop
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);

    // 2. Inicializar OLED
    oled_init_heltec();

    // 3. Wi-Fi 
    wifi_init_sta();

    // 4. Inicializar Driver RC522 
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

    // 5. Inicializar Scanner
    rc522_config_t scanner_config = {
        .driver = rfid_driver,
    };

    rc522_create(&scanner_config, &rfid_scanner);
    rc522_register_events(rfid_scanner, RC522_EVENT_PICC_STATE_CHANGED, on_rfid_event, NULL);
    rc522_start(rfid_scanner);

    ESP_LOGI(TAG, "Sistema de Manutencao Iniciado");

    oled = ssd1306_create(I2C_MASTER_NUM, 0x3C);
    ssd1306_clear_screen(oled, 0x00);
    ssd1306_draw_string(oled, 0, 0, (const uint8_t*)"Sistema Iniciado", 12, 1);
    ssd1306_refresh_gram(oled);
}