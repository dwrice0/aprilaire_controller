/* UART Events Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "bme280.h"
#include "i2c_bus.h"
#include "provisioning.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "aprilaire";

#define RESET_BUTTON_GPIO GPIO_NUM_10
#define RGB_LED_GPIO        GPIO_NUM_8
#define RGB_LED_RMT_CHANNEL RMT_CHANNEL_0
/* Calibration offsets — adjust based on your measurements */
#define RH_OFFSET    3.0f   /* % — typical range: 2.0 to 5.0 */
#define TEMP_OFFSET  1.27f  /* °C — typical range: 0.5 to 2.0 */

/* Configuration */
#define WIFI_SSID               CONFIG_WIFI_SSID
#define WIFI_PASSWORD           CONFIG_WIFI_PASSWORD
#define MQTT_BROKER_URI         CONFIG_MQTT_BROKER_URI
#define MQTT_TOPIC_STATUS       "aprilaire/status"
#define MQTT_TOPIC_RAW          "aprilaire/raw"
#define MQTT_TOPIC_SET          "aprilaire/set"
//#define MQTT_TOPIC_CALIBRATE    "aprilaire/calibrate"
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_MAXIMUM_RETRY      5

/* UART */
#define EX_UART_NUM LP_UART_NUM_0
#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)

/* Protocol constants */
#define STX 0x02
#define ETX 0x03

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define FRAME_M_UPDATED  BIT0
//#define FRAME_R_UPDATED  BIT1

#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_SDA_IO   GPIO_NUM_6
#define I2C_MASTER_SCL_IO   GPIO_NUM_7
#define I2C_MASTER_FREQ_HZ  100000
#define BME280_ADDR         BME280_I2C_ADDRESS_DEFAULT  /* 0x77 */
#define BME280_POLL_MS      5000

/* Queues / handles */
static QueueHandle_t uart0_queue;
static QueueHandle_t mqtt_queue;
static esp_mqtt_client_handle_t mqtt_client = NULL;
static EventGroupHandle_t       s_wifi_event_group;
static int                      s_retry_num = 0;

/* RS485 Packet Structure */
struct rs485packet {
    uint8_t *buf;
    size_t len;
};

/* Sensor state */
typedef struct {
    bool m_running;         // true if M! (dehumidifier running), false if M? (idle)
    int m_rh;               // RH from M-frame (controller's RH reading, live only during run)
    bool r_on;              // true if E070 is on (R[0]=0x01), false if off (R[0]=0x00)
    int r_dryness;          // setpoint from R-frame (1-7)
    int r_rh;               // RH*10 from R-frame Model 76 sensor (divide by 10 for %)
    int r_temp;             // temp*10 from R-frame Model 76 sensor (divide by 10 for °C)
    bool r_temp_valid;      // true if sep=0x00, false if sensor initializing (sep=0x01)
    bool valid;
} aprilaire_state_t;

static aprilaire_state_t g_state = {
    .r_on = true,
    .r_dryness = 4,
    .r_rh = 500,
    .r_temp = 200,
    .r_temp_valid = false,
};

static portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

/* ═══════════════════════════════════════════════════════════════════════════
 * WS2812 minimal driver via RMT
 * ═══════════════════════════════════════════════════════════════════════════ */

static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_rmt_encoder = NULL;

/* WS2812 timing (in RMT ticks at 10MHz = 100ns per tick) */
#define T0H  4   /* 400ns */
#define T0L  8   /* 800ns */
#define T1H  8   /* 800ns */
#define T1L  4   /* 400ns */
#define TRESET 500 /* >50us reset */

static void ws2812_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = RGB_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10000000,  /* 10MHz = 100ns per tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    rmt_new_tx_channel(&tx_cfg, &s_rmt_chan);

    rmt_copy_encoder_config_t enc_cfg = {};
    rmt_new_copy_encoder(&enc_cfg, &s_rmt_encoder);

    rmt_enable(s_rmt_chan);
}

static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt_chan || !s_rmt_encoder) return;

    /* WS2812 expects GRB order */
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;

    rmt_symbol_word_t symbols[24 + 1];
    for (int i = 23; i >= 0; i--) {
        if (grb & (1 << i)) {
            symbols[23 - i] = (rmt_symbol_word_t){
                .level0    = 1, .duration0 = T1H,
                .level1    = 0, .duration1 = T1L,
            };
        } else {
            symbols[23 - i] = (rmt_symbol_word_t){
                .level0    = 1, .duration0 = T0H,
                .level1    = 0, .duration1 = T0L,
            };
        }
    }
    /* Reset symbol */
    symbols[24] = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = TRESET,
        .level1 = 0, .duration1 = TRESET,
    };

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_rmt_chan, s_rmt_encoder, symbols,
                 sizeof(symbols), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(100));
}

static void ws2812_off(void)
{
    ws2812_set_color(0, 0, 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Reset button check
 * ═══════════════════════════════════════════════════════════════════════════ */

static void check_reset_button(void)
{
    gpio_set_direction(RESET_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RESET_BUTTON_GPIO, GPIO_PULLUP_ONLY);

    /* Not pressed at boot — nothing to do */
    if (gpio_get_level(RESET_BUTTON_GPIO) != 0) return;

    ws2812_init();
    ESP_LOGI(TAG, "Reset button held — keep holding for 3 seconds...");

    /* Flash yellow while counting */
    int held_ms = 0;
    bool led_state = false;
    while (gpio_get_level(RESET_BUTTON_GPIO) == 0 && held_ms < 3000) {
        led_state = !led_state;
        ws2812_set_color(led_state ? 32 : 0,
                         led_state ? 32 : 0,
                         0);  /* dim yellow flash */
        vTaskDelay(pdMS_TO_TICKS(250));
        held_ms += 250;
    }

    if (held_ms < 3000) {
        /* Released too early — ignore */
        ESP_LOGI(TAG, "Button released early — ignoring");
        ws2812_off();
        return;
    }

    /* Held 3 seconds — solid green, wait for release */
    ESP_LOGI(TAG, "3 seconds reached — release button to clear credentials");
    ws2812_set_color(32, 0, 0);  /* solid green */

    while (gpio_get_level(RESET_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Released — flash green 3 times to confirm, then clear */
    for (int i = 0; i < 3; i++) {
        ws2812_set_color(0, 64, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        ws2812_off();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "Clearing Wi-Fi credentials");
    provisioning_clear();
    esp_restart();
}

static uint8_t calc_checksum(const char *data, size_t len)
{
    uint32_t sum = 2;
    for (size_t i = 0; i < len; i++) {
        sum += (uint8_t)data[i];
    }
    return (uint8_t)(sum % 256);
}

static uint8_t parse_packet(const uint8_t *buf, size_t len, aprilaire_state_t *state)
{
    uint8_t updated = 0;
    const uint8_t *p   = buf;
    const uint8_t *end = buf + len;

    while (p < end) {
        if (*p != STX) { p++; continue; }
        p++;
        if (p >= end) break;

        const uint8_t *frame_start = p;
        while (p < end && *p != ETX) p++;
        size_t frame_len = (size_t)(p - frame_start);
        if (p < end) p++;
        if (frame_len < 2) continue;

        if (frame_start[0] == 'M' && frame_len >= 4) {
            char m_cmd = (char)frame_start[1];
            char m_rh_str[3] = {
                (char)frame_start[2],
                (char)frame_start[3],
                '\0'
            };
            int m_rh = (int)strtol(m_rh_str, NULL, 16);
            if ((m_cmd == '?' || m_cmd == '!') && m_rh > 0 && m_rh <= 100) {
                state->m_rh      = m_rh;
                state->m_running = (m_cmd == '!');
                state->valid     = true;
                updated         |= FRAME_M_UPDATED;
                ESP_LOGI(TAG, "M-frame: cmd=%c rh=%d%% running=%s",
                         m_cmd, m_rh, state->m_running ? "YES" : "NO");
            } else {
                ESP_LOGW(TAG, "M-frame: invalid fields cmd=%c rh_raw=%s", m_cmd, m_rh_str);
            }
        }
/*
        else if (frame_start[0] == 'R' && frame_len >= 13) {
            char r_on_str[3]   = { (char)frame_start[1],  (char)frame_start[2],  '\0' };
            char r_dry_str[3]  = { (char)frame_start[3],  (char)frame_start[4],  '\0' };
            char r_rh_str[5]   = { (char)frame_start[5],  (char)frame_start[6],
                                   (char)frame_start[7],  (char)frame_start[8],  '\0' };
            char r_sep_str[3]  = { (char)frame_start[9],  (char)frame_start[10], '\0' };
            char r_temp_str[3] = { (char)frame_start[11], (char)frame_start[12], '\0' };

            int r_on      = (int)strtol(r_on_str,   NULL, 16);
            int r_dryness = (int)strtol(r_dry_str,  NULL, 16);
            int r_rh      = (int)strtol(r_rh_str,   NULL, 16);
            int r_sep     = (int)strtol(r_sep_str,  NULL, 16);
            int r_temp    = (int)strtol(r_temp_str, NULL, 16);

            bool r_updated = false;

            if (r_on == 0x00 || r_on == 0x01) {
                state->r_on = (r_on == 0x01);
                r_updated = true;
            } else {
                ESP_LOGW(TAG, "R-frame: invalid r_on=0x%02X", r_on);
            }
            if (r_dryness >= 1 && r_dryness <= 7) {
                state->r_dryness = r_dryness;
                r_updated = true;
            } else {
                ESP_LOGW(TAG, "R-frame: invalid dryness=%d", r_dryness);
            }
            if (r_rh > 0 && r_rh <= 1000) {
                state->r_rh = r_rh;
                r_updated = true;
            } else {
                ESP_LOGW(TAG, "R-frame: invalid r_rh=%d", r_rh);
            }
            state->r_temp_valid = (r_sep == 0x00);
            if (state->r_temp_valid) {
                if (r_temp > 0 && r_temp <= 500) {
                    state->r_temp = r_temp;
                    r_updated = true;
                } else {
                    ESP_LOGW(TAG, "R-frame: invalid r_temp=%d", r_temp);
                }
            }
            if (r_updated) {
                updated |= FRAME_R_UPDATED;
                ESP_LOGI(TAG, "R-frame: on=%s dryness=%d rh=%.1f%% temp=%.1f°C temp_valid=%s",
                         state->r_on ? "true" : "false", r_dryness,
                         r_rh / 10.0f, r_temp / 10.0f,
                         state->r_temp_valid ? "true" : "false");
            }
        }
*/
        else {
            ESP_LOGW(TAG, "Unknown or truncated frame type=0x%02X len=%zu",
                     frame_start[0], frame_len);
        }
    }

    return updated;
}

static esp_err_t send_r_frame(void)
{
    bool    r_on;
    int     r_dryness, r_rh, r_temp;
    bool    r_temp_valid;

    taskENTER_CRITICAL(&g_state_mux);
    r_on         = g_state.r_on;
    r_dryness    = g_state.r_dryness;
    r_rh         = g_state.r_rh;
    r_temp       = g_state.r_temp;
    r_temp_valid = g_state.r_temp_valid;
    taskEXIT_CRITICAL(&g_state_mux);

    /* Apply calibration offsets to BME280 readings
     * rh_offset   = +3.0%   (BME280 reads 3.0% low vs Model 76)
     * temp_offset = +1.27°C (BME280 reads 1.27°C low vs Model 76)
     * Values are stored as *10 integers so offsets are scaled accordingly */
    if (r_temp_valid) {
        r_rh   = r_rh   + (int)(RH_OFFSET  * 10.0f);   /* +30 counts */
        r_temp = r_temp + (int)(TEMP_OFFSET * 10.0f);    /* +12 counts */

        /* Clamp to valid ranges */
        r_rh   = r_rh   < 0    ? 0    : r_rh   > 1000 ? 1000 : r_rh;
        r_temp = r_temp < 0    ? 0    : r_temp > 500  ? 500  : r_temp;
    }

    /* Frame body: R [on 2] [dryness 2] [rh 4] [sep 2] [temp 2] */
    char body[16];
    snprintf(body, sizeof(body), "R%02X%02X%04X%02X%02X",
             r_on         ? 0x01 : 0x00,
             r_dryness,
             r_rh,
             r_temp_valid ? 0x00 : 0x01,
             r_temp_valid ? r_temp : 0x00);

    uint8_t cs = calc_checksum(body, strlen(body));

    /* Full frame: STX + body + checksum + ETX */
    char frame[20];
    int flen = snprintf(frame, sizeof(frame), "%c%s%02X%c",
                        STX, body, cs, ETX);
    if (flen <= 0 || flen >= (int)sizeof(frame)) {
        ESP_LOGE(TAG, "send_r_frame: formatting error");
        return ESP_FAIL;
    }

    int written = uart_write_bytes(EX_UART_NUM, frame, flen);
    if (written < 0) {
        ESP_LOGE(TAG, "send_r_frame: uart_write_bytes failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "R-frame sent: on=%s dryness=%d rh=%.1f%% "
             "temp_valid=%s temp=%.1f°C cs=0x%02X",
             r_on         ? "YES" : "NO",
             r_dryness,
             r_rh   / 10.0f,
             r_temp_valid ? "YES" : "NO",
             r_temp / 10.0f,
             cs);

    return ESP_OK;
}
 
static void handle_set_command(const char *payload, int payload_len)
{
    char buf[128];
    int copy_len = payload_len < (int)(sizeof(buf) - 1)
                   ? payload_len : (int)(sizeof(buf) - 1);
    memcpy(buf, payload, copy_len);
    buf[copy_len] = '\0';

    /* Parse outside critical section */
    bool new_on = g_state.r_on;
    int  new_dryness = g_state.r_dryness;

    char *p = strstr(buf, "\"on\"");
    if (p) {
        p += 4;
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        if (strncmp(p, "true", 4) == 0)       new_on = true;
        else if (strncmp(p, "false", 5) == 0)  new_on = false;
    }

    p = strstr(buf, "\"dryness\"");
    if (p) {
        p += 9;
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        int d = atoi(p);
        if (d >= 1 && d <= 7) new_dryness = d;
        else ESP_LOGW(TAG, "SET: dryness %d out of range 1-7", d);
    }

    /* Write atomically */
    taskENTER_CRITICAL(&g_state_mux);
    g_state.r_on      = new_on;
    g_state.r_dryness = new_dryness;
    taskEXIT_CRITICAL(&g_state_mux);

    ESP_LOGI(TAG, "SET: on=%s dryness=%d", new_on ? "true" : "false", new_dryness);
}

static void mqtt_publish_discovery(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char device_id[13];
    snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Device block — shared across all entities */
    char device[320];
    snprintf(device, sizeof(device),
             "\"device\": {"
             "\"identifiers\": [\"aprilaire_e070_%s\"], "
             "\"name\": \"AprilAire E070 Dehumidifier\", "
             "\"model\": \"E070\", "
             "\"manufacturer\": \"DR\", "
             "\"sw_version\": \"0.0.1\""
             "}",
             device_id);

    char topic[128];
    char payload[768];

    /* ── m_rh — E070 RH sensor ────────────────────────────────────────── */
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/aprilaire_e070_%s/m_rh/config", device_id);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\": \"Dehumidifier RH\", "
             "\"unique_id\": \"aprilaire_e070_%s_m_rh\", "
             "\"state_topic\": \"aprilaire/status\", "
             "\"value_template\": \"{{ value_json.m_rh }}\", "
             "\"unit_of_measurement\": \"%%\", "
             "\"device_class\": \"humidity\", "
             "\"state_class\": \"measurement\", "
             "%s}",
             device_id, device);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    /* ── m_running — E070 running state ──────────────────────────────── */
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/aprilaire_e070_%s/m_running/config",
             device_id);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\": \"Dehumidifier Running\", "
             "\"unique_id\": \"aprilaire_e070_%s_m_running\", "
             "\"state_topic\": \"aprilaire/status\", "
             //"\"value_template\": \"{{ value_json.m_running }}\", "
             "\"value_template\": \"{{ value_json.m_running | string | lower }}\", "
             "\"payload_on\": \"true\", "
             "\"payload_off\": \"false\", "
             "\"device_class\": \"running\", "
             "%s}",
             device_id, device);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    /* ── r_on — power switch ─────────────────────────────────────────── */
    snprintf(topic, sizeof(topic),
             "homeassistant/switch/aprilaire_e070_%s/r_on/config", device_id);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\": \"Dehumidifier Power\", "
             "\"unique_id\": \"aprilaire_e070_%s_r_on\", "
             "\"state_topic\": \"aprilaire/status\", "
             "\"value_template\": \"{{ value_json.r_on }}\", "
             "\"command_topic\": \"aprilaire/set\", "
             "\"payload_on\": \"{\\\"on\\\": true}\", "
             "\"payload_off\": \"{\\\"on\\\": false}\", "
             "\"state_on\": \"true\", "
             "\"state_off\": \"false\", "
             "%s}",
             device_id, device);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    /* ── r_dryness — dryness setpoint number ────────────────────────── */
    snprintf(topic, sizeof(topic),
             "homeassistant/number/aprilaire_e070_%s/r_dryness/config",
             device_id);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\": \"Dryness Setpoint\", "
             "\"unique_id\": \"aprilaire_e070_%s_r_dryness\", "
             "\"state_topic\": \"aprilaire/status\", "
             "\"value_template\": \"{{ value_json.r_dryness }}\", "
             "\"command_topic\": \"aprilaire/set\", "
             "\"command_template\": \"{\\\"dryness\\\": {{ value | int }}}\", "
             "\"min\": 1, "
             "\"max\": 7, "
             "\"step\": 1, "
             "\"mode\": \"slider\", "
             "%s}",
             device_id, device);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    /* ── r_rh — controller RH sensor ───────────────────────────────── */
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/aprilaire_e070_%s/r_rh/config", device_id);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\": \"Controller RH\", "
             "\"unique_id\": \"aprilaire_e070_%s_r_rh\", "
             "\"state_topic\": \"aprilaire/status\", "
             "\"value_template\": \"{{ value_json.r_rh }}\", "
             "\"unit_of_measurement\": \"%%\", "
             "\"device_class\": \"humidity\", "
             "\"state_class\": \"measurement\", "
             "%s}",
             device_id, device);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    /* ── r_temp — controller temperature sensor ─────────────────────── */
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/aprilaire_e070_%s/r_temp/config",
             device_id);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\": \"Controller Temperature\", "
             "\"unique_id\": \"aprilaire_e070_%s_r_temp\", "
             "\"state_topic\": \"aprilaire/status\", "
             "\"value_template\": \"{{ value_json.r_temp }}\", "
             "\"unit_of_measurement\": \"°C\", "
             "\"device_class\": \"temperature\", "
             "\"state_class\": \"measurement\", "
             "%s}",
             device_id, device);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    /* ── r_temp_valid — temperature sensor validity ─────────────────── */
snprintf(topic, sizeof(topic),
         "homeassistant/sensor/aprilaire_e070_%s/r_temp_valid/config",
         device_id);
snprintf(payload, sizeof(payload),
         "{"
         "\"name\": \"Temperature Valid\", "
         "\"unique_id\": \"aprilaire_e070_%s_r_temp_valid\", "
         "\"state_topic\": \"aprilaire/status\", "
         "\"value_template\": \"{{ value_json.r_temp_valid | string | lower }}\", "
         "\"entity_category\": \"diagnostic\", "
         "%s}",
         device_id, device);
esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    ESP_LOGI(TAG, "MQTT discovery published for device aprilaire_e070_%s",
             device_id);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_SET, 1);
            ESP_LOGI(TAG, "Subscribed to %s", MQTT_TOPIC_SET);
            mqtt_publish_discovery();
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected — will retry");
            break;
        case MQTT_EVENT_DATA:
            if (event->topic_len > 0 &&
                strncmp(event->topic, MQTT_TOPIC_SET,
                    event->topic_len) == 0) {
                handle_set_command(event->data, event->data_len);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
        default:
            break;
    }
}
 
static void mqtt_publish_state(const aprilaire_state_t *state)
{
    if (!mqtt_client || !state->valid) return;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{"
             "\"m_rh\": %d, "
             "\"m_running\": %s, "
             "\"r_on\": %s, "
             "\"r_dryness\": %d, "
             "\"r_rh\": %.1f, "
             "\"r_temp\": %.1f, "
             "\"r_temp_valid\": %s"
             "}",
             state->m_rh,
             state->m_running ? "true" : "false",
             state->r_on ? "true" : "false",
             state->r_dryness,
             state->r_rh   / 10.0f,
             state->r_temp / 10.0f,
             state->r_temp_valid ? "true" : "false");

    int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS,
                                         payload, 0,
                                         /*qos=*/1, /*retain=*/1);
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "MQTT published: %s", payload);
    } else {
        ESP_LOGE(TAG, "MQTT publish failed (not connected yet?)");
    }
}

void mqtt_publish_raw_packet(const uint8_t *buf, size_t len)
{
    if (!mqtt_client) return;

    int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_RAW,
                                         (const char*)buf, len, 0, 1);
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "MQTT published raw packet, size: %d", len);
    } else {
        ESP_LOGE(TAG, "MQTT publish failed (not connected yet?)");
    }
}

static void mqtt_publisher_task(void *pvParameters)
{
    struct rs485packet pkt;
    aprilaire_state_t  prev = {0};
 
    for (;;) {
        if (xQueueReceive(mqtt_queue, &pkt, portMAX_DELAY) != pdTRUE) continue;
 
        ESP_LOGI(TAG, "Received packet from UART queue, size: %d", pkt.len);
        mqtt_publish_raw_packet(pkt.buf, pkt.len);
        free(pkt.buf);
 
        aprilaire_state_t cur;
        taskENTER_CRITICAL(&g_state_mux);
        cur = g_state;
        taskEXIT_CRITICAL(&g_state_mux);

        if (cur.valid && (cur.m_rh      != prev.m_rh      ||
                        cur.r_on      != prev.r_on      ||
                        cur.r_dryness != prev.r_dryness  ||
                        cur.m_running != prev.m_running ||
                        cur.r_rh      != prev.r_rh       ||
                        cur.r_temp    != prev.r_temp ||
                        cur.r_temp_valid != prev.r_temp_valid)) {
            mqtt_publish_state(&cur);
            prev = cur;
        }
    }
}

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
    uint8_t* rbuf = (uint8_t*) malloc(RD_BUF_SIZE);  /* reassembly buffer */
    size_t   rlen = 0;
    assert(dtmp && rbuf);

    for (;;) {
        if (xQueueReceive(uart0_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);
            switch (event.type) {
            case UART_DATA:
                ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);

                /* Append to reassembly buffer */
                if (rlen + event.size <= RD_BUF_SIZE) {
                    memcpy(rbuf + rlen, dtmp, event.size);
                    rlen += event.size;
                } else {
                    ESP_LOGW(TAG, "Reassembly buffer overflow, resetting");
                    rlen = 0;
                }

                /* Only process when we have a complete frame (ends with ETX) */
                if (rlen == 0 || rbuf[rlen - 1] != ETX) {
                    ESP_LOGI(TAG, "Waiting for more data (rlen=%d)", rlen);
                    break;
                }

                /* Have complete frame(s) — parse and reset */
                aprilaire_state_t new_state;
                taskENTER_CRITICAL(&g_state_mux);
                new_state = g_state;
                taskEXIT_CRITICAL(&g_state_mux);

                uint8_t frames = parse_packet(rbuf, rlen, &new_state);

                if (frames & FRAME_M_UPDATED) {
                    taskENTER_CRITICAL(&g_state_mux);
                    g_state.m_rh      = new_state.m_rh;
                    g_state.m_running = new_state.m_running;
                    g_state.valid     = true;
                    taskEXIT_CRITICAL(&g_state_mux);

                    vTaskDelay(pdMS_TO_TICKS(2));
                    send_r_frame();
                }
/*
                if (frames & FRAME_R_UPDATED) {
                    taskENTER_CRITICAL(&g_state_mux);
                    g_state.r_on         = new_state.r_on;
                    g_state.r_dryness    = new_state.r_dryness;
                    g_state.r_rh         = new_state.r_rh;
                    g_state.r_temp       = new_state.r_temp;
                    g_state.r_temp_valid = new_state.r_temp_valid;
                    taskEXIT_CRITICAL(&g_state_mux);
                }
*/
                struct rs485packet pkt = {
                    .len = rlen,
                    .buf = (uint8_t*) malloc(rlen),
                };
                if (!pkt.buf) {
                    ESP_LOGE(TAG, "malloc failed, dropping packet");
                } else {
                    memcpy(pkt.buf, rbuf, rlen);
                    if (xQueueSend(mqtt_queue, &pkt, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "mqtt_queue full, dropping packet");
                        free(pkt.buf);
                    }
                }

                rlen = 0;  /* reset reassembly buffer */
                break;

            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "hw fifo overflow");
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                rlen = 0;
                break;
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "ring buffer full");
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                rlen = 0;
                break;
            case UART_PARITY_ERR:
                ESP_LOGW(TAG, "uart parity error");
                break;
            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "uart frame error");
                rlen = 0;
                break;
            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }
    }

    free(dtmp);
    free(rbuf);
    vTaskDelete(NULL);
}

void uart_init(void)
{
    ESP_LOGI(TAG, "Initializing UART%d...", EX_UART_NUM);
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = LP_UART_SCLK_DEFAULT,
    };
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);

    uart_set_pin(EX_UART_NUM, GPIO_NUM_5, GPIO_NUM_4, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
//    uart_set_rx_full_threshold(EX_UART_NUM, 1);
    uart_set_rx_timeout(EX_UART_NUM, 10);

    xTaskCreate(uart_event_task, "uart_event_task", 3072, NULL, 12, NULL);

    ESP_LOGI(TAG, "UART%d: TX=GPIO%d RX=GPIO%d @ 9600 baud",
             EX_UART_NUM, GPIO_NUM_5, GPIO_NUM_4);
}

static void mqtt_init(void)
{
    char mqtt_host[64] = {0};
    char mqtt_user[64] = {0};
    char mqtt_pass[64] = {0};
    int  mqtt_port     = 1883;

    provisioning_get_mqtt_config(mqtt_host, sizeof(mqtt_host),
                                 &mqtt_port,
                                 mqtt_user, sizeof(mqtt_user),
                                 mqtt_pass, sizeof(mqtt_pass));

    char broker_uri[96];
    snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", mqtt_host, mqtt_port);

    mqtt_queue = xQueueCreate(20, sizeof(struct rs485packet));
    xTaskCreate(mqtt_publisher_task, "mqtt_pub_task", 4096, NULL, 10, NULL);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                  = broker_uri,
        .credentials.username                = mqtt_user,
        .credentials.authentication.password = mqtt_pass,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI(TAG, "MQTT broker: %s", broker_uri);
    ESP_LOGI(TAG, "  Status topic : %s", MQTT_TOPIC_STATUS);
    ESP_LOGI(TAG, "  Set topic    : %s", MQTT_TOPIC_SET);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)...",
                     s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Wi-Fi connection failed after %d retries",
                     WIFI_MAXIMUM_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
 
static bool wifi_init_sta_with_credentials(const char *ssid, const char *password)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL,
                                        &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL,
                                        &instance_got_ip);

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid,     ssid,     sizeof(wifi_config.sta.ssid)     - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", ssid);
        return true;
    }

    ESP_LOGE(TAG, "Failed to connect to AP: %s", ssid);
    return false;
}

static void bme280_task(void *pvParameters)
{
    i2c_config_t conf = {
        .mode            = I2C_MODE_MASTER,
        .sda_io_num      = I2C_MASTER_SDA_IO,
        .sda_pullup_en   = GPIO_PULLUP_ENABLE,
        .scl_io_num      = I2C_MASTER_SCL_IO,
        .scl_pullup_en   = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    i2c_bus_handle_t i2c_bus = i2c_bus_create(I2C_MASTER_NUM, &conf);
    if (!i2c_bus) {
        ESP_LOGE(TAG, "BME280: i2c_bus_create failed");
        vTaskDelete(NULL);
        return;
    }

    bme280_handle_t bme280 = bme280_create(i2c_bus, BME280_ADDR);
    if (!bme280) {
        ESP_LOGE(TAG, "BME280: bme280_create failed");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = bme280_default_init(bme280);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BME280: bme280_default_init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "BME280: initialized on SDA=GPIO%d SCL=GPIO%d addr=0x%02X",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, BME280_ADDR);
    vTaskDelay(pdMS_TO_TICKS(100));
    for (;;) {
        float temperature = 0.0f, humidity = 0.0f;

        esp_err_t t_ret = bme280_read_temperature(bme280, &temperature);
        esp_err_t h_ret = bme280_read_humidity(bme280, &humidity);

        if (t_ret == ESP_OK && h_ret == ESP_OK) {
            /* Convert to *10 integer format expected by send_r_frame() */
            int r_rh   = (int)(humidity    * 10.0f);
            int r_temp = (int)(temperature * 10.0f);

            /* Clamp to valid ranges */
            r_rh   = r_rh   < 0    ? 0    : r_rh   > 1000 ? 1000 : r_rh;
            r_temp = r_temp < 0    ? 0    : r_temp > 500  ? 500  : r_temp;


            taskENTER_CRITICAL(&g_state_mux);
            g_state.r_rh         = r_rh;
            g_state.r_temp       = r_temp;
            g_state.r_temp_valid = true;
            taskEXIT_CRITICAL(&g_state_mux);

            ESP_LOGI(TAG, "BME280: rh=%.1f%% temp=%.1f°C", humidity, temperature);
            /* Publish calibration data for offset characterization */
/*
            if (mqtt_client) {
                aprilaire_state_t snap;
                taskENTER_CRITICAL(&g_state_mux);
                snap = g_state;
                taskEXIT_CRITICAL(&g_state_mux);

                float model76_rh   = snap.r_rh   / 10.0f;
                float model76_temp = snap.r_temp / 10.0f;
                float diff_rh      = humidity    - model76_rh;
                float diff_temp    = temperature - model76_temp;

                char payload[256];
                snprintf(payload, sizeof(payload),
                         "{"
                         "\"bme280_rh\": %.1f, "
                         "\"bme280_temp\": %.1f, "
                         "\"model76_rh\": %.1f, "
                         "\"model76_temp\": %.1f, "
                         "\"diff_rh\": %.1f, "
                         "\"diff_temp\": %.1f"
                         "}",
                         humidity, temperature,
                         model76_rh, model76_temp,
                         diff_rh, diff_temp);

                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_CALIBRATE,
                                payload, 0, 1, 0);
                ESP_LOGI(TAG, "Calibrate: %s", payload);
            }
*/
        } else {
            ESP_LOGW(TAG, "BME280: read failed (t=%s h=%s)",
                     esp_err_to_name(t_ret), esp_err_to_name(h_ret));
        }

        vTaskDelay(pdMS_TO_TICKS(BME280_POLL_MS));
    }
}

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "Starting AprilAire RS485 Controller...");
    nvs_flash_init();
    ESP_LOGI(TAG, "ESP-NVS initialized");
    esp_netif_init();
    ESP_LOGI(TAG, "ESP-NETIF initialized");
    esp_event_loop_create_default();
    ESP_LOGI(TAG, "ESP-EventLoop initialized");

    /* Check if RESET_BUTTON_GPIO button held on startup → clear credentials */
    check_reset_button();

    /* Check if provisioning is needed */
    if (provisioning_is_needed()) {
        ESP_LOGI(TAG, "No Wi-Fi credentials found — starting provisioning");
        provisioning_start();
        /* provisioning_start() calls esp_restart() — never returns */
    }

    /* Load credentials from NVS and connect */
    char ssid[64]     = {0};
    char password[64] = {0};
    provisioning_get_credentials(ssid, sizeof(ssid),
                                 password, sizeof(password));

//    if (wifi_init_sta()) {
    if (wifi_init_sta_with_credentials(ssid, password)) {
        ws2812_off();
        sleep(2);
        mqtt_init();
        xTaskCreate(bme280_task, "bme280_task", 4096, NULL, 5, NULL);
        uart_init();
    }
}
