#include "provisioning.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

static const char *TAG = "provisioning";

#define PROV_AP_SSID        "Aprilaire-Setup"
#define PROV_AP_PASS        ""              /* open AP */
#define PROV_AP_IP          "192.168.4.1"
#define NVS_NAMESPACE       "wifi_creds"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "password"
#define NVS_KEY_MQTT_HOST   "mqtt_host"
#define NVS_KEY_MQTT_PORT   "mqtt_port"
#define NVS_KEY_MQTT_USER   "mqtt_user"
#define NVS_KEY_MQTT_PASS   "mqtt_pass"
#define PROV_DONE_BIT       BIT0

static EventGroupHandle_t s_prov_event_group;
static char s_ssid[64]     = {0};
static char s_password[64] = {0};

/* ── HTML portal page ───────────────────────────────────────────────────── */
static const char *PORTAL_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Aprilaire Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;}"
    "h2{color:#1a73e8;}h3{color:#555;font-size:14px;margin:16px 0 4px;}"
    "input{width:100%;padding:10px;margin:4px 0 8px;box-sizing:border-box;"
    "border:1px solid #ccc;border-radius:4px;font-size:16px;}"
    "button{width:100%;padding:12px;background:#1a73e8;color:white;border:none;"
    "border-radius:4px;font-size:16px;cursor:pointer;margin-top:8px;}"
    "button:hover{background:#1557b0;}"
    ".divider{border-top:1px solid #eee;margin:16px 0;}"
    ".hint{font-size:12px;color:#888;margin:-4px 0 8px;}"
    "</style></head><body>"
    "<h2>Aprilaire E070 Setup</h2>"
    "<form method='POST' action='/save'>"
    "<h3>Wi-Fi</h3>"
    "<input type='text'     name='ssid'     placeholder='Wi-Fi network name' required>"
    "<input type='password' name='password' placeholder='Wi-Fi password'>"
    "<div class='divider'></div>"
    "<h3>MQTT Broker</h3>"
    "<p class='hint'>Your Home Assistant MQTT broker address</p>"
    "<input type='text' name='mqtt_host' placeholder='192.168.1.x or hostname' required>"
    "<input type='number' name='mqtt_port' placeholder='Port (default 1883)' value='1883'>"
    "<input type='text'     name='mqtt_user' placeholder='MQTT username (if required)'>"
    "<input type='password' name='mqtt_pass' placeholder='MQTT password (if required)'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form>"
    "</body></html>";

static const char *SUCCESS_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Aprilaire Setup</title>"
    "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;}"
    "h2{color:#137333;}.box{background:#e6f4ea;padding:20px;border-radius:8px;}"
    "</style></head><body>"
    "<h2>&#10003; Connected!</h2>"
    "<div class='box'>"
    "<p>Credentials saved successfully.</p>"
    "<p>The device will now restart and connect to your Wi-Fi network.</p>"
    "<p>You can close this page and disconnect from <strong>Aprilaire-Setup</strong>.</p>"
    "</div></body></html>";

/* ── NVS helpers ────────────────────────────────────────────────────────── */
bool provisioning_is_needed(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return true;

    char ssid[64]      = {0};
    char mqtt_host[64] = {0};
    size_t ssid_len      = sizeof(ssid);
    size_t mqtt_host_len = sizeof(mqtt_host);

    bool needs_prov =
        nvs_get_str(nvs, NVS_KEY_SSID,      ssid,      &ssid_len)      != ESP_OK ||
        nvs_get_str(nvs, NVS_KEY_MQTT_HOST, mqtt_host, &mqtt_host_len) != ESP_OK ||
        strlen(ssid) == 0 || strlen(mqtt_host) == 0;

    nvs_close(nvs);
    return needs_prov;
}

bool provisioning_get_mqtt_config(char *host, size_t host_len,
                                  int  *port,
                                  char *user, size_t user_len,
                                  char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    bool ok = true;
    int32_t stored_port = 1883;

    if (nvs_get_str(nvs, NVS_KEY_MQTT_HOST, host, &host_len) != ESP_OK) ok = false;
    nvs_get_i32(nvs, NVS_KEY_MQTT_PORT, &stored_port);
    *port = (int)stored_port;
    nvs_get_str(nvs, NVS_KEY_MQTT_USER, user, &user_len);
    nvs_get_str(nvs, NVS_KEY_MQTT_PASS, pass, &pass_len);

    nvs_close(nvs);
    return ok;
}

bool provisioning_get_credentials(char *ssid, size_t ssid_len,
                                  char *password, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    bool ok = true;
    if (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len) != ESP_OK)     ok = false;
    if (nvs_get_str(nvs, NVS_KEY_PASS, password, &pass_len) != ESP_OK) ok = false;
    nvs_close(nvs);
    return ok;
}

void provisioning_clear(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Wi-Fi credentials cleared");
}

static esp_err_t save_credentials(const char *ssid, const char *password,
                                   const char *mqtt_host, int mqtt_port,
                                   const char *mqtt_user, const char *mqtt_pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, NVS_KEY_SSID,      ssid);
    nvs_set_str(nvs, NVS_KEY_PASS,      password);
    nvs_set_str(nvs, NVS_KEY_MQTT_HOST, mqtt_host);
    nvs_set_i32(nvs, NVS_KEY_MQTT_PORT, mqtt_port);
    nvs_set_str(nvs, NVS_KEY_MQTT_USER, mqtt_user);
    nvs_set_str(nvs, NVS_KEY_MQTT_PASS, mqtt_pass);

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/* ── URL decode helper ──────────────────────────────────────────────────── */
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t out = 0;
    while (*src && out < dst_len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[out++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[out++] = ' ';
            src++;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}

static void parse_form_field(const char *body, const char *key,
                             char *out, size_t out_len)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    char encoded[128] = {0};
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, p, len);
    url_decode(out, encoded, out_len);
}

/* ── HTTP handlers ──────────────────────────────────────────────────────── */
static esp_err_t handler_root(httpd_req_t *req)
{
    /* Redirect all requests to the portal — captive portal magic */
    const char *host = NULL;
    char host_buf[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host",
                                    host_buf, sizeof(host_buf)) == ESP_OK) {
        host = host_buf;
    }

    /* If not already pointing at our IP, redirect */
    if (host && strcmp(host, PROV_AP_IP) != 0) {
        char redirect[128];
        snprintf(redirect, sizeof(redirect), "http://%s/", PROV_AP_IP);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", redirect);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, strlen(PORTAL_HTML));
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[512] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[ret] = '\0';

    char ssid[64]      = {0};
    char password[64]  = {0};
    char mqtt_host[64] = {0};
    char mqtt_port[8]  = {0};
    char mqtt_user[64] = {0};
    char mqtt_pass[64] = {0};

    parse_form_field(body, "ssid",      ssid,      sizeof(ssid));
    parse_form_field(body, "password",  password,  sizeof(password));
    parse_form_field(body, "mqtt_host", mqtt_host, sizeof(mqtt_host));
    parse_form_field(body, "mqtt_port", mqtt_port, sizeof(mqtt_port));
    parse_form_field(body, "mqtt_user", mqtt_user, sizeof(mqtt_user));
    parse_form_field(body, "mqtt_pass", mqtt_pass, sizeof(mqtt_pass));

    if (strlen(ssid) == 0 || strlen(mqtt_host) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "SSID and MQTT host required", 27);
        return ESP_FAIL;
    }

    int port = strlen(mqtt_port) > 0 ? atoi(mqtt_port) : 1883;

    if (save_credentials(ssid, password, mqtt_host, port,
                         mqtt_user, mqtt_pass) == ESP_OK) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, SUCCESS_HTML, strlen(SUCCESS_HTML));
        xEventGroupSetBits(s_prov_event_group, PROV_DONE_BIT);
    } else {
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

/* Catch-all handler for captive portal detection endpoints */
static esp_err_t handler_captive(httpd_req_t *req)
{
    char redirect[64];
    snprintf(redirect, sizeof(redirect), "http://%s/", PROV_AP_IP);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", redirect);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── DNS task — redirects all DNS queries to our IP ────────────────────── */
static void dns_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&src, &src_len);
        if (len < 12) continue;

        /* Build minimal DNS response pointing to our IP */
        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x81; resp[3] = 0x80;  /* QR=1, OPCODE=0, AA=1, RA=1 */
        resp[6] = 0x00; resp[7] = 0x01;  /* ANCOUNT = 1 */

        int offset = len;
        /* Answer: pointer to question name */
        resp[offset++] = 0xC0; resp[offset++] = 0x0C;
        /* Type A, Class IN */
        resp[offset++] = 0x00; resp[offset++] = 0x01;
        resp[offset++] = 0x00; resp[offset++] = 0x01;
        /* TTL = 60s */
        resp[offset++] = 0x00; resp[offset++] = 0x00;
        resp[offset++] = 0x00; resp[offset++] = 0x3C;
        /* RDLENGTH = 4 */
        resp[offset++] = 0x00; resp[offset++] = 0x04;
        /* RDATA = 192.168.4.1 */
        resp[offset++] = 192; resp[offset++] = 168;
        resp[offset++] = 4;   resp[offset++] = 1;

        sendto(sock, resp, offset, 0,
               (struct sockaddr *)&src, src_len);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ── Main provisioning entry point ─────────────────────────────────────── */
void provisioning_start(void)
{
    s_prov_event_group = xEventGroupCreate();

    /* Start SoftAP */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap_config = {
        .ap = {
            .ssid            = PROV_AP_SSID,
            .ssid_len        = strlen(PROV_AP_SSID),
            .password        = PROV_AP_PASS,
            .max_connection  = 4,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Provisioning AP started: SSID=%s IP=%s",
             PROV_AP_SSID, PROV_AP_IP);

    /* Start DNS task for captive portal */
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL);

    /* Start HTTP server */
    httpd_handle_t server = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_start(&server, &http_cfg);

    httpd_uri_t uri_root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handler_root,
    };
    httpd_uri_t uri_save = {
        .uri      = "/save",
        .method   = HTTP_POST,
        .handler  = handler_save,
    };
    /* Captive portal detection endpoints for various OSes */
    httpd_uri_t uri_captive = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = handler_captive,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_save);
    httpd_register_uri_handler(server, &uri_captive);

    /* Wait for credentials to be submitted */
    ESP_LOGI(TAG, "Waiting for Wi-Fi credentials via captive portal...");
    xEventGroupWaitBits(s_prov_event_group, PROV_DONE_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "Credentials received — SSID: %s", s_ssid);

    /* Small delay to ensure HTTP response is sent before stopping */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Clean up and restart */
    httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_deinit();

    ESP_LOGI(TAG, "Restarting to connect with new credentials...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}