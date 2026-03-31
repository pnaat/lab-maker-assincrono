#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_adc_cal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

//--------------------------------------------------------------------------------------

#define DEFAULT_VREF          1100                                //Vref Default
#define ADC_BITS              12                                  //Quantity of bit ESP32 ADC1
#define ADC_COUNTS            (1 << ADC_BITS)                     //Left Shift Raized 2 times .: ADC_COUNTS = 24
#define ADC_CHANNEL ADC1_CHANNEL_6 // ADC1 channel connected to GPIO34
#define ADC_UNIT ADC_UNIT_1        //ADC Unit

#define WIFI_MAX_RETRY      5


#if CONFIG_BROKER_CERTIFICATE_OVERRIDDEN == 1
static const uint8_t mqtt_eclipseprojects_io_pem_start[]  = "-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
#else
extern const uint8_t mqtt_eclipseprojects_io_pem_start[]   asm("_binary_mqtt_eclipseprojects_io_pem_start");
#endif
extern const uint8_t mqtt_eclipseprojects_io_pem_end[]   asm("_binary_mqtt_eclipseprojects_io_pem_end");


//--------------------------------------------------------------------------------------
// Declarações de Variaveis
//--------------------------------------------------------------------------------------
int sampleI;
unsigned int samples;

double Irms;
double filteredI;                //Filtered_ is the raw analog value minus the DC offset
double offsetI;                  //Low-pass filter output
double ICAL;                     //Calibration coefficient. Need to be set in order to obtain accurate results
double sqI, sumI;

float kwhValue = 0.0;
float sumKwh = 0.0;
float sumCost = 0.0;
float costKwh = 0.0;

//--------------------------------------------------------------------------------------
// Protótipos das funções de calculo de Irms
//--------------------------------------------------------------------------------------
void currentCalibration(double _ICAL);
double getIrms(int NUMBER_OF_SAMPLES);

//--------------------------------------------------------------------------------------
/*handle do Queue*/
//--------------------------------------------------------------------------------------

QueueHandle_t xSensor_Control = 0;

//--------------------------------------------------------------------------------------
/* Variáveis para Armazenar o handle da Task */
//--------------------------------------------------------------------------------------
TaskHandle_t xPublishTask;
TaskHandle_t xSensorTask;

//--------------------------------------------------------------------------------------
/*Prototipos das Tasks*/
//--------------------------------------------------------------------------------------
void vPublishTask(void *pvParameter);
void vSensorTask(void *pvParameter);

//--------------------------------------------------------------------------------------
//Inicializacao de TAGs
//--------------------------------------------------------------------------------------
static const char *TAG = "MQTT_IOT";
static const char *TAG1 = "TASK";
static const char *TAG2 = "sensor";

//--------------------------------------------------------------------------------------
// MQTT
//--------------------------------------------------------------------------------------
const char* irms_topic = "home/sensor/irms";
const char* kwh_topic = "home/sensor/kwh";
const char* cost_topic = "home/sensor/cost";
char mqtt_buffer[128];

//--------------------------------------------------------------------------------------
/* estrutura de dados para o sensor */
//--------------------------------------------------------------------------------------
struct sensor
{
  double irms;
  float kwh;
  float cost;
} sensorCurrent;


//--------------------------------------------------------------------------------------
//Referencia para saber status de conexao
//--------------------------------------------------------------------------------------

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

//--------------------------------------------------------------------------------------
//Cliente MQTT
//--------------------------------------------------------------------------------------

static esp_mqtt_client_handle_t s_mqtt = NULL;

//--------------------------------------------------------------------------------------
// Funcao Calibracao do Sensor de Corrente
//--------------------------------------------------------------------------------------

void currentCalibration(double _ICAL)
{

  ICAL = _ICAL;
  offsetI = ADC_COUNTS >> 1;
  int adjust = 0;
  
  while (adjust < 250)
  {
    getIrms(1480);
    adjust++;

  }

}

//--------------------------------------------------------------------------------------
// Funcao de acquisicao do valor Irms
//--------------------------------------------------------------------------------------

double getIrms(int NUMBER_OF_SAMPLES)
{

  samples = NUMBER_OF_SAMPLES;
  int SupplyVoltage = 3300; //3V3 power supply ESP32

  sampleI = 0;

  for (unsigned int n = 0; n < samples; n++)
  {
    sampleI = adc1_get_raw(ADC_CHANNEL);

    // O filtro passa-baixa digital extrai o 1.65 VDC offset,
    //subtraia isso - o sinal agora está centrado em 0 contagens.
    offsetI = (offsetI + (sampleI - offsetI) / ADC_COUNTS);
    filteredI = sampleI - offsetI;

    // Corrente RMS
    // 1) square current values
    sqI = filteredI * filteredI;
    // 2) sum
    sumI += sqI;
  }

  double I_RATIO = ICAL * ((SupplyVoltage / 1000.0) / (ADC_COUNTS));
  Irms = I_RATIO * sqrt(sumI / samples);

  //Reset accumulators
  sumI = 0;
  //--------------------------------------------------------------------------------------

  return Irms;
}


//--------------------------------------------------------------------------------------
/* Task de Publish MQTT */
//--------------------------------------------------------------------------------------
void vPublishTask(void *pvParameter)
{

  struct sensor sensorReceived;

  while(1)
  {

    if (!xQueueReceive(xSensor_Control, (void *)&sensorReceived, 3000))
    {
      ESP_LOGI(TAG, "Falha ao receber o valor da fila xSensor_Control.\n");
    }
    
    //JSON FORMAT IF NECESSARY: "{\"current\":\"%lf\"}"

    ESP_LOGI(TAG, "Enviando dados para o topico %s...", irms_topic);
    //Sanity check do mqtt_client antes de publicar
    snprintf(mqtt_buffer, 128, "%lf", sensorReceived.irms);
    esp_mqtt_client_publish(s_mqtt, irms_topic, mqtt_buffer, 0, 0, 0);

    ESP_LOGI(TAG, "Enviando dados para o topico %s...", kwh_topic);
    //Sanity check do mqtt_client antes de publicar
    snprintf(mqtt_buffer, 128, "%f", sensorReceived.kwh);
    esp_mqtt_client_publish(s_mqtt, kwh_topic, mqtt_buffer, 0, 0, 0);

    ESP_LOGI(TAG, "Enviando dados para o topico %s...", cost_topic);
    //Sanity check do mqtt_client antes de publicar
    snprintf(mqtt_buffer, 128, "%f", sensorReceived.cost);
    esp_mqtt_client_publish(s_mqtt, cost_topic, mqtt_buffer, 0, 0, 0);

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

//--------------------------------------------------------------------------------------
/* Task do sensor SCT013 */
//--------------------------------------------------------------------------------------
void vSensorTask(void *pvParameter)
{
  
  ESP_LOGI(TAG, "Iniciando task leitura sensor");

  currentCalibration(23);
  
  ESP_LOGI(TAG, "Iniciando task leitura sensor SCT013 50A/1V...");

  while (1)
  {

    ESP_LOGI(TAG, "Lendo dados de Consumo de Energia...\n");

    ESP_LOGI(TAG, "Calculando a corrente Irms...\n");
    sensorCurrent.irms = getIrms(1676);
    /*
    Os valores úteis para usar como parâmetro em calcIrms () são:

    1 ciclo:
    112 para sistemas 50 Hz , ou 93 para sistemas 60 Hz.
    
    O menor número ‘universal’:
    559 (100 ms, ou 5 ciclos de 50 Hz ou 6 ciclos de 60 Hz).

    O período de monitoramento recomendado:
    1676 (300 ms).
    */

    //Consumed = (Pot[W]/1000) x hours...Sent a packet in a interval of 5 seconds .: kwh = ((Irms * Volts * 5 seconds) / (1000 * 3600))
    ESP_LOGI(TAG, "Calculando KWh Consumido...\n");
    //sensorCurrent.kwh = (float)((sensorCurrent.irms * 127.0 * 5) / (1000.0 * 3600.0));
    kwhValue = (float)((sensorCurrent.irms * 127.0 * 5) / (1000.0 * 3600.0));

    ESP_LOGI(TAG, "Calculando Custo por KWh Consumido...\n");
    costKwh = kwhValue * 0.34; // Tarif 2026 R$  0,34

    sumKwh += kwhValue;
    sumCost += costKwh;

    sensorCurrent.kwh = sumKwh;
    sensorCurrent.cost = sumCost;

    ESP_LOGI(TAG2, "Read IRMS Value: %lf (A) | kWh Value: %f (kWh) | Cost per kWh : R$ %f", sensorCurrent.irms, sensorCurrent.kwh, sensorCurrent.cost);

    if (!xQueueSend(xSensor_Control, (void *)&sensorCurrent, 3000))
    {
      ESP_LOGI(TAG, "\nFalha ao enviar o valor para a fila xSensor_Control.\n");
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}


// ===================== WIFI ======================

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
    strncpy((char*)wifi_config.sta.ssid, CONFIG_ESP_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, CONFIG_ESP_WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando ao WiFi SSID:%s", CONFIG_ESP_WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// ===================== MQTT ======================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado");
            break;
        default:
            break;
    }
}


static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_ESP_MQTT_URL,
        .broker.verification.certificate = (const char *)mqtt_eclipseprojects_io_pem_start,
        .credentials.username = CONFIG_ESP_MQTT_USER,
        .credentials.authentication.password = CONFIG_ESP_MQTT_PASS,
        .session.disable_clean_session = false,
        .session.keepalive = 30,
    };
    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

//--------------------------------------------------------------------------------------
//APP_MAIN
//--------------------------------------------------------------------------------------
void app_main()
{

  //Configure ADC

  adc1_config_width(ADC_WIDTH_BIT_12); // 12-bit ADC width
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11); 
  
  ESP_LOGI(TAG, "Iniciando ESP32 IoT App...");
 
  // Setup de logs de outros elementos
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

  // Inicializacao da NVS = Non-Volatile-Storage (NVS)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init_sta();
  mqtt_start();

  //criação de fila do xSensor_Control (vSensorTask <--> vPublishTask)
  xSensor_Control = xQueueCreate(10, sizeof(struct sensor));
  if (xSensor_Control == NULL)
  {
    ESP_LOGI(TAG1, "Erro na criação da Queue.\n");
  }


  if (xTaskCreate(&vPublishTask, "vPublishTask", configMINIMAL_STACK_SIZE + 4096, NULL, 5, NULL) != pdTRUE)
  {
    ESP_LOGE("Erro", "error - nao foi possivel alocar vPublishTask.\n");
    while (1);
  }

  if (xTaskCreate(vSensorTask, "vSensorTask", configMINIMAL_STACK_SIZE + 4096, NULL, 5, NULL) != pdTRUE)
  {
    ESP_LOGE("Erro", "error - nao foi possivel alocar vSensorTask.\n");
    while (1);
  }

  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(3000)); // Delay
  }

}
