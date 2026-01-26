#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"


#define TXD    7
#define RXD    6
#define RTS    UART_PIN_NO_CHANGE
#define CTS    UART_PIN_NO_CHANGE

#define GPOWER   19

#define UART_PORT       1 
#define UART_BAUD_RATE  9600
//#define UART_BAUD_RATE  57600
//#define UART_BAUD_RATE  38400
#define TASK_STACK      2048  

#define BUF_SIZE (1024)

#define TIME_ZONE (-6)  
#define YEAR_BASE (2000) 

static const char *TAG = "gps";

void parse_gpgga(const char *nmea_sentence, float *latitude, float *longitude) {
    char *token;
    char buffer[128];
    strncpy(buffer, nmea_sentence, sizeof(buffer));

    // Tokenize the sentence using ',' as the delimiter
    token = strtok(buffer, ",");
    int field_index = 0;
    float lat = 0.0, lon = 0.0;
    char lat_dir = 'N', lon_dir = 'E';

    while (token != NULL) {
        field_index++;

        if (field_index == 3) {
            lat = atof(token); // Latitude
        } else if (field_index == 4) {
            lat_dir = token[0]; // Latitude direction (N/S)
        } else if (field_index == 5) {
            lon = atof(token); // Longitude
        } else if (field_index == 6) {
            lon_dir = token[0]; // Longitude direction (E/W)
        }

        token = strtok(NULL, ",");
    }

    // Convert latitude and longitude to decimal degrees
    *latitude = ((int)(lat / 100) + (lat - ((int)(lat / 100) * 100)) / 60.0) * (lat_dir == 'S' ? -1 : 1);
    *longitude = ((int)(lon / 100) + (lon - ((int)(lon / 100) * 100)) / 60.0) * (lon_dir == 'W' ? -1 : 1);
}


static void uart_task(void *arg)
{

    gpio_set_level(GPOWER , 0);

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << GPOWER,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;
    //intr_alloc_flags = ESP_INTR_FLAG_IRAM;

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TXD, RXD, RTS, CTS));

    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    gpio_set_level(GPOWER , 1);
    while (1) {

        int len = uart_read_bytes(UART_PORT, data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        if (len) {
            char *gpgga_start = strstr((char *)data, "$GPGGA");
            if (gpgga_start) {
                char *end = strchr(gpgga_start, '\n');
                if (end) {
                    *end = '\0';
                    float latitude = 0.0, longitude = 0.0;
                    parse_gpgga(gpgga_start, &latitude, &longitude);
                    ESP_LOGI(TAG, "Parsed Latitude: %.6f, Longitude: %.6f", latitude, longitude);

                    // Update GPS coordinates to the server
                    //update_gps_data(latitude, longitude); -> https://github.com/johannajes/GPS_ESP32/blob/main/main/http_server.c
                }
            }
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    xTaskCreate(uart_task, "uart_task", TASK_STACK, NULL, 10, NULL);

}
