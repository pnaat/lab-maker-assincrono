#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"
#include "ssd1306.h"

static const char *TAG = "CADASTRO_RFID";

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

ssd1306_handle_t oled = NULL;
static rc522_driver_handle_t driver;
static rc522_handle_t scanner;

void setup_display() {
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
    ssd1306_draw_string(oled, 0, 0, (const uint8_t*)"SISTEMA PRONTO", 12, 1);
    ssd1306_refresh_gram(oled);
}

static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        char uid_str[20];
        // Na v4.0, o print oficial é rc522_picc_print, mas para o OLED fazemos assim:
        snprintf(uid_str, sizeof(uid_str), "%02X%02X%02X%02X", 
                 picc->uid.value[0], picc->uid.value[1], 
                 picc->uid.value[2], picc->uid.value[3]);

        ESP_LOGI(TAG, "Tag detectada: %s", uid_str);

        ssd1306_clear_screen(oled, 0x00);
        ssd1306_draw_string(oled, 0, 0, (const uint8_t*)"UID LIDO:", 12, 1);
        ssd1306_draw_string(oled, 0, 20, (const uint8_t*)uid_str, 12, 1);
        ssd1306_refresh_gram(oled);
    }
}

void app_main() {
    esp_event_loop_create_default();
    setup_display();

    // 1. Configurar o Driver SPI (Padrão v4.0)
    rc522_spi_config_t spi_config = {
        .host_id = SPI2_HOST, // SPI2 é o padrão para o S3
        .bus_config = &(spi_bus_config_t){
            .miso_io_num = RFID_MISO,
            .mosi_io_num = RFID_MOSI,
            .sclk_io_num = RFID_SCK,
        },
        .dev_config = {
            .spics_io_num = RFID_SDA,
        },
        .rst_io_num = -1, // Soft-reset
    };

    // 2. Criar e Instalar o Driver
    rc522_spi_create(&spi_config, &driver);
    rc522_driver_install(driver);

    // 3. Configurar o Scanner usando o driver criado
    rc522_config_t scanner_config = {
        .driver = driver,
    };

    rc522_create(&scanner_config, &scanner);
    rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
    rc522_start(scanner);
}