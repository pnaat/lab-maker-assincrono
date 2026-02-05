#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include "soc/rtc.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt_client.h"
#include "bme280.h"

static const char *TAG = "WIFI_STATION";
#define ESP_WIFI_SSID      "ENTER_SSID"
#define ESP_WIFI_PASS      "ENTER_PWD"
#define ESP_MAXIMUM_RETRY  5
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static int s_retry_num = 0;

#define MQTT_PUB_TEMP_BME280 "bme280/temperature"
#define MQTT_PUB_HUM_BME280 "bme280/humidity"
#define MQTT_PUB_PRES_BME280 "bme280/pressure"

#define SDA_PIN 21
#define SCL_PIN 22
#define I2C_MASTER_ACK 0
#define I2C_MASTER_NACK 1

static const char *SLEEPTAG = "SLEEP_WAKEUP";
#define WAKEUP_TIMEOUT 10
static RTC_DATA_ATTR struct timeval sleep_enter_time;

static char TEMPDATA[32] = {0};;
#define MAX_DEVICES         8
#define SAMPLE_PERIOD       1000 // ms

#define TAG_BME280 "BME280"

uint32_t MQTT_CONNEECTED = 0;

char* LastcharDel(char* name) {
  int i = 0;
  while(name[i] != '\0') {
    i++;
  }
  name[i-1] = '\0';
  return name;
}

void i2c_master_init()
{
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 1000000};
    i2c_param_config(I2C_NUM_0, &i2c_config);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        MQTT_CONNEECTED = 1;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        MQTT_CONNEECTED = 0;
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

esp_mqtt_client_handle_t client = NULL;
static void mqtt_app_start(void)
{
    ESP_LOGI(TAG, "STARTING MQTT");
    esp_mqtt_client_config_t mqttConfig = {0}; 
    mqttConfig.broker.address.uri = "mqtt://192.168.1.3:1883";

    client = esp_mqtt_client_init(&mqttConfig);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

s8 BME280_I2C_bus_read(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
    s32 iError = BME280_INIT_VALUE;
    esp_err_t espRc;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);

    if (cnt > 1)
    {
        i2c_master_read(cmd, reg_data, cnt - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, reg_data + cnt - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
    if (espRc == ESP_OK)
    {
        iError = SUCCESS;
    }
    else
    {
        iError = ESP_FAIL;
    }

    i2c_cmd_link_delete(cmd);

    return (s8)iError;
}

s8 BME280_I2C_bus_write(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
    s32 iError = BME280_INIT_VALUE;

    esp_err_t espRc;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);

    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write(cmd, reg_data, cnt, true);
    i2c_master_stop(cmd);

    espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
    if (espRc == ESP_OK)
    {
        iError = SUCCESS;
    }
    else
    {
        iError = ESP_FAIL;
    }
    i2c_cmd_link_delete(cmd);

    return (s8)iError;
}

void BME280_delay_msek(u32 msek)
{
    vTaskDelay(msek / portTICK_PERIOD_MS);
}

void Publisher_Task()
{
    struct bme280_t bme280 = {
        .bus_read = BME280_I2C_bus_read,
        .dev_addr = BME280_I2C_ADDRESS1,
        .delay_msec = BME280_delay_msek};

    s32 com_rslt;
    s32 v_uncomp_pressure_s32;
    s32 v_uncomp_temperature_s32;
    s32 v_uncomp_humidity_s32;

    com_rslt = bme280_init(&bme280);

    com_rslt += bme280_set_oversamp_pressure(BME280_OVERSAMP_16X);
    com_rslt += bme280_set_oversamp_temperature(BME280_OVERSAMP_2X);
    com_rslt += bme280_set_oversamp_humidity(BME280_OVERSAMP_1X);

    com_rslt += bme280_set_standby_durn(BME280_STANDBY_TIME_1_MS);
    com_rslt += bme280_set_filter(BME280_FILTER_COEFF_16);

    com_rslt += bme280_set_power_mode(BME280_NORMAL_MODE);
    if (com_rslt == SUCCESS)
    {
      vTaskDelay(40 / portTICK_PERIOD_MS);

      com_rslt = bme280_read_uncomp_pressure_temperature_humidity(
          &v_uncomp_pressure_s32, &v_uncomp_temperature_s32, &v_uncomp_humidity_s32);

      double temp = bme280_compensate_temperature_double(v_uncomp_temperature_s32);
      char temperature[12];
      sprintf(temperature, "%.2f degC", temp);

      double press = bme280_compensate_pressure_double(v_uncomp_pressure_s32) / 100; // Pa -> hPa
      char pressure[10];
      sprintf(pressure, "%.2f hPa", press);

      double hum = bme280_compensate_humidity_double(v_uncomp_humidity_s32);
      char humidity[10];
      sprintf(humidity, "%.2f %%", hum);

      if (com_rslt == SUCCESS)
      {
          if (MQTT_CONNEECTED)
          {
              esp_mqtt_client_publish(client, MQTT_PUB_TEMP_BME280, temperature, 0, 0, 0);
              esp_mqtt_client_publish(client, MQTT_PUB_PRES_BME280, pressure, 0, 0, 0);
              esp_mqtt_client_publish(client, MQTT_PUB_HUM_BME280, humidity, 0, 0, 0);
              vTaskDelay(5000 / portTICK_PERIOD_MS);
          }
      }
      else
      {
          ESP_LOGE(TAG_BME280, "measure error. code: %d", com_rslt);
      }
        
    }
    else
    {
        ESP_LOGE(TAG_BME280, "init or setting error. code: %d", com_rslt);
    }
}

static void read_temperature_sensor() {
    struct bme280_t bme280 = {
        .bus_read   = BME280_I2C_bus_read,
        .dev_addr   = BME280_I2C_ADDRESS1,
        .delay_msec = BME280_delay_msek
    };

    s32 com_rslt;
    s32 v_uncomp_pressure_s32 = 0;
    s32 v_uncomp_temperature_s32 = 0;
    s32 v_uncomp_humidity_s32 = 0;

    // Inicializa o BME280
    com_rslt = bme280_init(&bme280);

    // Mantive as mesmas configs usadas no seu Publisher_Task()
    com_rslt += bme280_set_oversamp_pressure(BME280_OVERSAMP_16X);
    com_rslt += bme280_set_oversamp_temperature(BME280_OVERSAMP_2X);
    com_rslt += bme280_set_oversamp_humidity(BME280_OVERSAMP_1X);
    com_rslt += bme280_set_standby_durn(BME280_STANDBY_TIME_1_MS);
    com_rslt += bme280_set_filter(BME280_FILTER_COEFF_16);
    com_rslt += bme280_set_power_mode(BME280_NORMAL_MODE);

    if (com_rslt != SUCCESS) {
        ESP_LOGE(TAG_BME280, "BME280 init/config error. code: %d", com_rslt);
        snprintf(TEMPDATA, sizeof(TEMPDATA), "N/A");
        return;
    }

    // Aguardinha curta para conversão
    vTaskDelay(40 / portTICK_PERIOD_MS);

    // Leitura não compensada e compensação
    com_rslt = bme280_read_uncomp_pressure_temperature_humidity(
        &v_uncomp_pressure_s32, &v_uncomp_temperature_s32, &v_uncomp_humidity_s32);

    if (com_rslt != SUCCESS) {
        ESP_LOGE(TAG_BME280, "measure error. code: %d", com_rslt);
        snprintf(TEMPDATA, sizeof(TEMPDATA), "N/A");
        return;
    }

    double temp_c = bme280_compensate_temperature_double(v_uncomp_temperature_s32);
    // Grava no buffer TEMPDATA para o log existente na connected_task
    snprintf(TEMPDATA, sizeof(TEMPDATA), "%.2f degC", temp_c);
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

/**
 * Called when Wifi and IP-address is OK.
 * 
 * This is the function to put online tasks into.
 * */
static void connected_task(void *pvParameters) {

  read_temperature_sensor(); // Read temperature data

  ESP_LOGI("DEBUG", "Current value from tempsensor: %s", TEMPDATA);

  Publisher_Task();

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
      ESP_LOGI(TAG, "retry to connect to the AP");
    }
    ESP_LOGI(TAG,"connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "got ip");
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    mqtt_app_start();
    xTaskCreate(&connected_task, "connected_task", 8192, NULL, 5, NULL); // Runs once wifi is up, and ip is OK.
  }
}

/**
 * Inits a Wifi-station and connects to the network defined in menuconfig
 * */
static void wifi_init_sta() {
  ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
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

  ESP_LOGI(TAG, "wifi_init_sta finished.");
  ESP_LOGI(TAG, "connect to ap SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
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

  i2c_master_init();

  wifi_init_sta(); // Runs connected_task() once everything is up an running

  ESP_LOGI("DEBUG", "End of Script?");
}