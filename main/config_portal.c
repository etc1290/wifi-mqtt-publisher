/**
 * @file config_portal.c
 * @brief WiFi AP mode + HTTP web server for device configuration
 *
 * Starts an open AP ("ESP32-Config") at 192.168.4.1 and serves a
 * mobile-friendly HTML form for WiFi & MQTT settings. On submit,
 * saves to NVS and restarts the device.
 */

#include "config_portal.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "PORTAL";

/* Event group to signal when config has been saved */
static EventGroupHandle_t s_portal_event_group;
#define CONFIG_SAVED_BIT BIT0

/* Pointer to current config (for pre-filling form) */
static const device_config_t *s_current_config = NULL;
static const char *s_fail_reason = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
 *  URL-decode helper
 * ═══════════════════════════════════════════════════════════════════════ */

static char hex_to_char(char hi, char lo)
{
    char val = 0;
    if (hi >= '0' && hi <= '9')      val = (hi - '0') << 4;
    else if (hi >= 'A' && hi <= 'F') val = (hi - 'A' + 10) << 4;
    else if (hi >= 'a' && hi <= 'f') val = (hi - 'a' + 10) << 4;

    if (lo >= '0' && lo <= '9')      val |= (lo - '0');
    else if (lo >= 'A' && lo <= 'F') val |= (lo - 'A' + 10);
    else if (lo >= 'a' && lo <= 'f') val |= (lo - 'a' + 10);
    return val;
}

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_len - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            dst[di++] = hex_to_char(src[si + 1], src[si + 2]);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Form field parser
 * ═══════════════════════════════════════════════════════════════════════ */

static esp_err_t parse_form_field(const char *body, const char *key,
                                  char *value, size_t value_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *start = strstr(body, search);
    if (!start) {
        value[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    start += strlen(search);

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    /* Temporary buffer for encoded value */
    char encoded[256] = {0};
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, start, len);
    encoded[len] = '\0';

    url_decode(value, encoded, value_len);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTML page (embedded)
 * ═══════════════════════════════════════════════════════════════════════ */

static const char HTML_PAGE_TEMPLATE[] =
"<!DOCTYPE html>"
"<html><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-C3 Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,-apple-system,sans-serif;"
"background:#0f0f1a;color:#e0e0e0;min-height:100vh;"
"display:flex;align-items:center;justify-content:center;padding:16px}"
".card{background:linear-gradient(145deg,#1a1a2e,#16213e);"
"border-radius:16px;padding:28px;width:100%%;max-width:420px;"
"box-shadow:0 8px 32px rgba(0,0,0,.4)}"
".logo{text-align:center;font-size:2em;margin-bottom:4px}"
"h1{text-align:center;font-size:1.2em;color:#8ecae6;margin-bottom:16px}"
".reason{background:#e945601a;border:1px solid #e94560;"
"border-radius:8px;padding:10px;text-align:center;"
"color:#e94560;font-size:.85em;margin-bottom:16px}"
"h2{font-size:.9em;color:#64dfdf;margin:18px 0 8px;"
"border-bottom:1px solid #ffffff15;padding-bottom:6px}"
"label{display:block;font-size:.82em;color:#888;margin:10px 0 3px}"
"input[type=text],input[type=password]{width:100%%;padding:10px 12px;"
"border:1px solid #ffffff20;border-radius:8px;background:#0f0f1a;"
"color:#fff;font-size:.95em;transition:border .2s}"
"input:focus{outline:none;border-color:#64dfdf}"
"button{width:100%%;padding:13px;margin-top:22px;"
"background:linear-gradient(135deg,#e94560,#c73651);"
"color:#fff;border:none;border-radius:10px;font-size:1.05em;"
"font-weight:600;cursor:pointer;transition:transform .15s}"
"button:active{transform:scale(.97)}"
".info{text-align:center;color:#555;font-size:.75em;margin-top:14px}"
"</style></head><body>"
"<div class='card'>"
"<div class='logo'>⚡</div>"
"<h1>ESP32-C3 Setup</h1>"
"%s"  /* reason banner (or empty) */
"<form action='/save' method='POST'>"
"<h2>📶 WiFi</h2>"
"<label>SSID</label>"
"<input type='text' name='ssid' value='%s' required>"
"<label>Password</label>"
"<input type='password' name='pass' value='%s'>"
"<h2>📡 MQTT Broker</h2>"
"<label>Broker URI</label>"
"<input type='text' name='broker' value='%s' placeholder='mqtt://host:1883' required>"
"<label>Topic</label>"
"<input type='text' name='topic' value='%s' required>"
"<label>Username (optional)</label>"
"<input type='text' name='mqtt_user' value='%s'>"
"<label>Password (optional)</label>"
"<input type='password' name='mqtt_pass' value='%s'>"
"<button type='submit'>💾 Save &amp; Restart</button>"
"</form>"
"<div class='info'>📍 http://192.168.4.1</div>"
"</div></body></html>";

static const char HTML_SUCCESS[] =
"<!DOCTYPE html>"
"<html><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Saved!</title>"
"<style>"
"body{font-family:system-ui;background:#0f0f1a;color:#e0e0e0;"
"display:flex;align-items:center;justify-content:center;height:100vh}"
".card{text-align:center;background:#1a1a2e;padding:40px;"
"border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,.4)}"
".icon{font-size:3em;margin-bottom:12px}"
"h1{color:#64dfdf;font-size:1.3em}"
"p{color:#888;margin-top:10px;font-size:.9em}"
"</style></head><body>"
"<div class='card'>"
"<div class='icon'>✅</div>"
"<h1>Configuration Saved!</h1>"
"<p>Device is restarting…</p>"
"</div></body></html>";

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP handlers
 * ═══════════════════════════════════════════════════════════════════════ */

static esp_err_t handle_get_root(httpd_req_t *req)
{
    char reason_html[256] = "";
    if (s_fail_reason && strlen(s_fail_reason) > 0) {
        snprintf(reason_html, sizeof(reason_html),
                 "<div class='reason'>⚠️ %s</div>", s_fail_reason);
    }

    /* Build page with current config values pre-filled */
    size_t page_size = sizeof(HTML_PAGE_TEMPLATE) + 1024;
    char *page = malloc(page_size);
    if (!page) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    snprintf(page, page_size, HTML_PAGE_TEMPLATE,
             reason_html,
             s_current_config->wifi_ssid,
             s_current_config->wifi_password,
             s_current_config->mqtt_broker_uri,
             s_current_config->mqtt_topic,
             s_current_config->mqtt_username,
             s_current_config->mqtt_password);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, strlen(page));
    free(page);

    return ESP_OK;
}

static esp_err_t handle_post_save(httpd_req_t *req)
{
    /* Read POST body */
    char body[512] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    ESP_LOGI(TAG, "Received config form (len=%d)", received);

    /* Parse form fields */
    device_config_t new_config = {0};
    parse_form_field(body, "ssid",      new_config.wifi_ssid,       sizeof(new_config.wifi_ssid));
    parse_form_field(body, "pass",      new_config.wifi_password,   sizeof(new_config.wifi_password));
    parse_form_field(body, "broker",    new_config.mqtt_broker_uri, sizeof(new_config.mqtt_broker_uri));
    parse_form_field(body, "topic",     new_config.mqtt_topic,      sizeof(new_config.mqtt_topic));
    parse_form_field(body, "mqtt_user", new_config.mqtt_username,   sizeof(new_config.mqtt_username));
    parse_form_field(body, "mqtt_pass", new_config.mqtt_password,   sizeof(new_config.mqtt_password));

    /* Save to NVS */
    esp_err_t err = config_save(&new_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✓ Config saved: SSID=%s Broker=%s Topic=%s",
             new_config.wifi_ssid, new_config.mqtt_broker_uri, new_config.mqtt_topic);

    /* Send success page */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_SUCCESS, strlen(HTML_SUCCESS));

    /* Signal that config was saved */
    xEventGroupSetBits(s_portal_event_group, CONFIG_SAVED_BIT);

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  WiFi AP mode
 * ═══════════════════════════════════════════════════════════════════════ */

static void wifi_start_ap(void)
{
    /* If WiFi was previously initialized (STA mode), stop and deinit first */
    esp_wifi_stop();
    esp_wifi_deinit();

    /* Create AP netif */
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Generate unique AP name from MAC address: ESP32-CFG-XXYYZZ */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP32-CFG-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    wifi_config_t ap_config = {
        .ap = {
            .channel        = 1,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "  📶 AP started: \"%s\"", ap_ssid);
    ESP_LOGI(TAG, "  🌐 Open http://192.168.4.1");
    ESP_LOGI(TAG, "═══════════════════════════════════════");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void config_portal_start(const device_config_t *current_config, const char *reason)
{
    s_current_config = current_config;
    s_fail_reason = reason;
    s_portal_event_group = xEventGroupCreate();

    /* ── Start AP ─────────────────────────────────────────────────────── */
    wifi_start_ap();

    /* ── Start HTTP server ────────────────────────────────────────────── */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size = 8192;
    httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    /* GET / → config form */
    httpd_uri_t uri_get = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handle_get_root,
    };
    httpd_register_uri_handler(server, &uri_get);

    /* POST /save → save & restart */
    httpd_uri_t uri_post = {
        .uri      = "/save",
        .method   = HTTP_POST,
        .handler  = handle_post_save,
    };
    httpd_register_uri_handler(server, &uri_post);

    ESP_LOGI(TAG, "HTTP server started on port %d", http_cfg.server_port);

    /* ── Wait for user to submit config ───────────────────────────────── */
    xEventGroupWaitBits(s_portal_event_group, CONFIG_SAVED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);

    /* Give browser time to receive the success page */
    ESP_LOGI(TAG, "Config saved – restarting in 2 seconds …");
    vTaskDelay(pdMS_TO_TICKS(2000));

    httpd_stop(server);
    esp_restart();
}
