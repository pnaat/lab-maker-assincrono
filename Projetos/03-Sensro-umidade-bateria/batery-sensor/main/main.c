#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"

#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include "soc/rtc.h"

#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

static const char *WIFITAG = "WIFI_STATION";
#define ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define ESP_MAXIMUM_RETRY  5
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static int s_retry_num = 0;

static const char *SLEEPTAG = "SLEEP_WAKEUP";
#define WAKEUP_TIMEOUT  CONFIG_WAKEUP_TIMEOUT
static RTC_DATA_ATTR struct timeval sleep_enter_time;

static const char TEMPDATA[] = "";
#define MAX_DEVICES         8
#define SAMPLE_PERIOD       1000 // ms

char* LastcharDel(char* name) {
  int i = 0;
  while(name[i] != '\0') {
    i++;
  }
  name[i-1] = '\0';
  return name;
}

static void read_temperature_sensor() {
  //implementar
}

/**
 * Sends the ESP32 off to sleep.
 * */
static void hibernate() {
  const int wakeup_time_sec = WAKEUP_TIMEOUT;
  ESP_LOGI(SLEEPTAG, "Enabling timer wakeup, %ds.", wakeup_time_sec);
  esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);

  ESP_LOGI(SLEEPTAG, "Entering deep sleep.");
  gettimeofday(&sleep_enter_time, NULL);

  esp_deep_sleep_start();
}


static void post_data() {
  // implementar
}

/**
 * Called when Wifi and IP-address is OK.
 * 
 * This is the function to put online tasks into.
 * */
static void connected_task(void *pvParameters) {

  read_temperature_sensor(); // Read temperature data

  ESP_LOGI("DEBUG", "Current value from tempsensor: %s", TEMPDATA);

  // Build our data-string:
  char buffer[1024];
  snprintf(buffer, sizeof(buffer),
    "{\"temperature\":[%s]}",
    LastcharDel(TEMPDATA)
  );

  post_data();

  hibernate();

  vTaskDelete(NULL);
}

/**
 * Handles the WiFi and IP events
 * */
static void _wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < ESP_MAXIMUM_RETRY) {
      esp_wifi_connect();
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      s_retry_num++;
      ESP_LOGI(WIFITAG, "retry to connect to the AP");
    }
    ESP_LOGI(WIFITAG,"connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(WIFITAG, "got ip");
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    xTaskCreate(&connected_task, "connected_task", 8192, NULL, 5, NULL); // Runs once wifi is up, and ip is OK.
  }
}

/**
 * Inits a Wifi-station and connects to the network defined in menuconfig
 * */
static void wifi_init_sta() {
  ESP_LOGI(WIFITAG, "ESP_WIFI_MODE_STA");
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL));

  wifi_config_t wifi_config = {
    .sta = {
      .ssid = ESP_WIFI_SSID,
      .password = ESP_WIFI_PASS
    },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
  ESP_ERROR_CHECK(esp_wifi_start() );

  ESP_LOGI(WIFITAG, "wifi_init_sta finished.");
  ESP_LOGI(WIFITAG, "connect to ap SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
}

/**
 * Reports the reason for waking up aswell as how long sleep lasted
 * */
static void report_wakeup_status() {
  struct timeval now;
  gettimeofday(&now, NULL);
  int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: {
      ESP_LOGI(SLEEPTAG,"Wake up from timer. Time spent in deep sleep: %dms\n", sleep_time_ms);
      break;
    }
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
      ESP_LOGI(SLEEPTAG,"Not a deep sleep reset\n");
  }
}

void app_main() {
  report_wakeup_status();

  // Required by WiFi:
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init_sta(); // Runs connected_task() once everything is up an running

  ESP_LOGI("DEBUG", "End of Script?");
}