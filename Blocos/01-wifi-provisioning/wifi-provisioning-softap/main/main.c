#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"

#include "nvs_flash.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

static const char *TAG = "WIFI_PROV";

/* ============================================================
 * Provisioning Event Handler
 * ============================================================ */
static void prov_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {

        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;

        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received SSID: %s",
                     (const char *)wifi_sta_cfg->ssid);
            break;
        }

        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            ESP_LOGI(TAG, "Connecting to Wi-Fi...");
            esp_wifi_connect();
            break;

        case WIFI_PROV_END:
            ESP_LOGI(TAG, "Provisioning ended");
            wifi_prov_mgr_deinit();
            break;

        default:
            break;
        }
    }
}

/* ============================================================
 * Wi-Fi Init (STA only)
 * ============================================================ */
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();  

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ============================================================
 * app_main
 * ============================================================ */
void app_main(void)
{
    bool provisioned = false;

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize Wi-Fi */
    wifi_init_sta();

    /* Check provisioning status */
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {

        ESP_LOGI(TAG, "Device NOT provisioned, starting SoftAP provisioning");

        wifi_prov_mgr_config_t config = {
            .scheme = wifi_prov_scheme_softap,
            .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
        };

        ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

        ESP_ERROR_CHECK(esp_event_handler_register(
            WIFI_PROV_EVENT,
            ESP_EVENT_ANY_ID,
            &prov_event_handler,
            NULL));

        const char *service_name = "ESP32_PROV";
        const char *service_key  = "12345678";

        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            WIFI_PROV_SECURITY_1,
            service_key,
            service_name,
            NULL
        ));

        ESP_LOGI(TAG, "Provisioning AP started");
        ESP_LOGI(TAG, "SSID: %s", service_name);
        ESP_LOGI(TAG, "PASS: %s", service_key);

    } else {

        ESP_LOGI(TAG, "Already provisioned, connecting to Wi-Fi");
        esp_wifi_connect();
    }
}
