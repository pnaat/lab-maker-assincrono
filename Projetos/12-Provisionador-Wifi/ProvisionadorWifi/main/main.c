#include <sys/param.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include <ctype.h>
#include "esp_http_server.h"
#include "dns_server.h"
#include "mdns.h"

#include "driver/gpio.h"

// --- Definições de Hardware e Wi-Fi ---
#define LED_GPIO GPIO_NUM_35 
static uint8_t led_state = 0;

#define EXAMPLE_ESP_WIFI_SSID "ESP32-Config"
#define EXAMPLE_ESP_WIFI_PASS "" // AP aberto para facilitar o acesso do usuário no primeiro uso
#define EXAMPLE_MAX_STA_CONN 4

// Ponteiros para o arquivo HTML embarcado via CMake (EMBED_FILES)
extern const char root_start[] asm("_binary_root_html_start");
extern const char root_end[] asm("_binary_root_html_end");

static const char *TAG = "captive_portal";
static bool is_connected = false;

/*
  Configura o pino do LED utilizando a API gpio_config_t (recomendada no ESP-IDF moderno).
 */
static void init_led_gpio(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, led_state);
}

/*
 * @brief Inicializa o serviço mDNS para resolver domínios na rede local sem precisar saber o IP estático.
 * Permite que clientes na mesma rede acessem o dispositivo via: http://esp32.local
 */
static void start_mdns_service(void) {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp32"));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 Control Webserver"));
    ESP_LOGI(TAG, "mDNS iniciado! Acesse na sua rede local por: http://esp32.local");
}

// ============================================================================
// --- Gerenciamento de Memória Não Volátil (NVS) ---
// ============================================================================

/*
 * @brief Salva as credenciais da rede Wi-Fi na partição NVS para persistência pós-reboot.
 */
static esp_err_t save_wifi_credentials(const char *ssid, const char *password) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_store", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "password", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs); // Confirma a gravação na flash
    }
    nvs_close(nvs);
    return err;
}

/*
 * @brief Carrega as credenciais salvas na NVS, caso existam.
 */
static esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_store", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "password", password, &pass_len);
    }
    nvs_close(nvs);
    return err;
}

// ============================================================================
// --- Event Loop & Configuração do Wi-Fi ---
// ============================================================================

/*
 * @brief Callback de eventos de sistema (WIFI_EVENT e IP_EVENT).
 * Trata o estado da conexão Station (STA) em segundo plano de forma assíncrona.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        is_connected = false;
        ESP_LOGW(TAG, "Conexao com a rede local perdida. Tentando reconectar...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Conectado ao Wi-Fi! IP local obtido: " IPSTR, IP2STR(&event->ip_info.ip));
        is_connected = true;
    }
}

/*
 * @brief Inicializa a pilha Wi-Fi no modo híbrido AP+STA (Access Point + Station).
 * - O modo SoftAP provê o Captive Portal (192.168.4.1).
 * - O modo STA conecta à rede Wi-Fi doméstica.
 */
static void wifi_init_apsta(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Registro de handlers no Event Loop do ESP-IDF
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configuração do Ponto de Acesso (SoftAP)
    wifi_config_t ap_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1, // Canal fixo é crucial para estabilidade em modo APSTA
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) > 0) {
        ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));

    // Configuração de IP Estático e Servidor DHCP para o SoftAP
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
        esp_netif_dhcps_start(ap_netif);
    }

    // Tenta carregar credenciais previamente salvas para conectar como Station
    char saved_ssid[32] = {0};
    char saved_pass[64] = {0};
    if (load_wifi_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)) == ESP_OK && strlen(saved_ssid) > 0) {
        ESP_LOGI(TAG, "Credenciais encontradas na NVS! Tentando conectar em: %s", saved_ssid);
        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, saved_ssid, sizeof(sta_config.sta.ssid));
        strncpy((char *)sta_config.sta.password, saved_pass, sizeof(saved_pass));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi APSTA iniciado. SSID do AP: '%s'", EXAMPLE_ESP_WIFI_SSID);
}

// ============================================================================
// --- Handlers do Servidor HTTP ---
// ============================================================================

/*
 * @brief GET /status: Retorna um JSON com o estado da conexão, IP obtido e status do LED.
 * Consultado via AJAX/fetch pelo frontend a cada 2 segundos.
 */
static esp_err_t status_get_handler(httpd_req_t *req) {
    char resp_str[256];
    
    if (is_connected) {
        wifi_ap_record_t ap_info;
        esp_wifi_sta_get_ap_info(&ap_info);

        // Obtém o IP atribuído à interface Station (STA)
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        char ip_addr[16] = "Desconhecido";

        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
        }

        snprintf(resp_str, sizeof(resp_str), 
                 "{\"connected\":true,\"ssid\":\"%s\",\"ip\":\"%s\",\"led\":%d}", 
                 ap_info.ssid, ip_addr, led_state);
    } else {
        snprintf(resp_str, sizeof(resp_str), 
                 "{\"connected\":false,\"ip\":\"0.0.0.0\",\"led\":%d}", led_state);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str);
    return ESP_OK;
}

/*
 * @brief POST /toggle-led: Altera o estado do pino GPIO do LED e retorna o novo estado via JSON.
 */
static esp_err_t led_post_handler(httpd_req_t *req) {
    char buf[16] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    
    if (ret > 0) {
        if (strstr(buf, "state=1")) {
            led_state = 1;
        } else if (strstr(buf, "state=0")) {
            led_state = 0;
        } else {
            led_state = !led_state;
        }
    } else {
        led_state = !led_state;
    }

    gpio_set_level(LED_GPIO, led_state);
    ESP_LOGI(TAG, "LED no GPIO %d alterado para: %d", LED_GPIO, led_state);

    const char *resp = led_state ? "{\"led\":1}" : "{\"led\":0}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    return ESP_OK;
}

/*
  @brief GET /: Entrega o arquivo HTML compilado no binário do firmware.
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
    const uint32_t root_len = root_end - root_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, root_start, root_len);
    return ESP_OK;
}

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= 'A' - 10;
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= 'A' - 10;
            else b -= '0';
            *dst = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            *dst = ' ';
            src++;
        } else {
            *dst = *src;
            src++;
        }
        dst++;
    }
    *dst = '\0';
}

/*
 * @brief POST /connect: Processa o formulário HTTP enviado pelo usuário,
 * extrai o SSID e senha, salva na NVS e dispara a reconexão como Station.
 */
static esp_err_t connect_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char raw_ssid[32] = {0};
    char raw_password[64] = {0};
    char ssid[32] = {0};
    char password[64] = {0};

    // Extrai os valores do corpo da requisição POST
    httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid));
    httpd_query_key_value(buf, "password", raw_password, sizeof(raw_password));

    // DECODIFICA OS CARACTERES ESPECIAIS (como o @)
    url_decode(ssid, raw_ssid);
    url_decode(password, raw_password);

    ESP_LOGI(TAG, "Recebido via POST -> SSID decodificado: %s", ssid);

    if (strlen(ssid) > 0) {
        // Salva a senha correta (já limpa e decodificada) na NVS
        save_wifi_credentials(ssid, password);

        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));

        esp_wifi_disconnect();
        esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config);
        esp_wifi_connect();

        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "<html><head><meta http-equiv='refresh' content='3;url=/'></head>"
                                "<body><h1>Credenciais Salvas!</h1><p>O ESP32 esta conectando...</p></body></html>");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID invalido");
    }

    return ESP_OK;
}

/*
 * @brief Handler de Erro 404 (Núcleo do Captive Portal).
 * Captura qualquer requisição HTTP para domínios externos (ex: google.com, apple.com/generate_204)
 * e responde com HTTP 302 Redirect para 192.168.4.1, forçando o SO móvel a abrir o portal.
 */
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
    ESP_LOGI(TAG, "Tentativa de acesso detectada na URI: %s", req->uri);

    // Resposta direcionada para testes do Captive Network Assistant (CNA) de iOS/Android
    if (strstr(req->uri, "generate_204") || strstr(req->uri, "hotspot-detect") || strstr(req->uri, "library/test")) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Redirecionamento genérico para chamadas fora do escopo
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "Redirecting to Captive Portal...");
    
    return ESP_OK;
}

/*
 * @brief Inicializa e registra as rotas do servidor HTTP (esp_http_server).
 */
static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 9;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri    = { .uri = "/",           .method = HTTP_GET,  .handler = root_get_handler };
        httpd_uri_t status_uri  = { .uri = "/status",     .method = HTTP_GET,  .handler = status_get_handler };
        httpd_uri_t connect_uri = { .uri = "/connect",    .method = HTTP_POST, .handler = connect_post_handler };
        httpd_uri_t led_uri     = { .uri = "/toggle-led", .method = HTTP_POST, .handler = led_post_handler };

        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &connect_uri);
        httpd_register_uri_handler(server, &led_uri);
        
        // Registra o interceptador de erro 404 para criar a redireção do Captive Portal
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
    }
    return server;
}

// ============================================================================
// --- Ponto de Entrada (app_main) ---
// ============================================================================

void app_main(void) {
    // Reduz o nível de log do servidor HTTP para evitar estouro de buffers por conta das requisições do portal
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    // 1. Inicializa a pilha de rede TCP/IP e o Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Inicializa a memória NVS (necessária para o Wi-Fi e para salvar credenciais)
    ESP_ERROR_CHECK(nvs_flash_init());

    // 3. Cria as interfaces padrão de rede (AP e STA) na pilha LwIP
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    // 4. Inicializa perifericos e serviços
    init_led_gpio();
    wifi_init_apsta();
    start_webserver();
    start_mdns_service();

    // 5. Inicia o Servidor DNS que responde a qualquer consulta "*" apontando para 192.168.4.1
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    start_dns_server(&config);
}