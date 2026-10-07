/**
 * @file config_store.c
 * @brief NVS-backed configuration storage implementation
 */

#include "config_store.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "CONFIG";
static const char *NVS_NAMESPACE = "dev_config";

/* NVS keys */
#define KEY_WIFI_SSID       "wifi_ssid"
#define KEY_WIFI_PASS       "wifi_pass"
#define KEY_MQTT_URI        "mqtt_uri"
#define KEY_MQTT_TOPIC      "mqtt_topic"
#define KEY_MQTT_USER       "mqtt_user"
#define KEY_MQTT_PASS       "mqtt_pass"

/* ── Helpers ──────────────────────────────────────────────────────────── */

static esp_err_t nvs_read_str(nvs_handle_t h, const char *key,
                              char *buf, size_t buf_len, const char *fallback)
{
    size_t len = buf_len;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(buf, fallback, buf_len);
        return ESP_OK;
    }
    return err;
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t config_load(device_config_t *config)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* No saved config → use Kconfig defaults */
        ESP_LOGI(TAG, "No saved config – using Kconfig defaults");
        strlcpy(config->wifi_ssid,       CONFIG_WIFI_SSID,       sizeof(config->wifi_ssid));
        strlcpy(config->wifi_password,   CONFIG_WIFI_PASSWORD,   sizeof(config->wifi_password));
        strlcpy(config->mqtt_broker_uri, CONFIG_MQTT_BROKER_URI, sizeof(config->mqtt_broker_uri));
        strlcpy(config->mqtt_topic,      CONFIG_MQTT_TOPIC,      sizeof(config->mqtt_topic));
        strlcpy(config->mqtt_username,   CONFIG_MQTT_USERNAME,   sizeof(config->mqtt_username));
        strlcpy(config->mqtt_password,   CONFIG_MQTT_PASSWORD,   sizeof(config->mqtt_password));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    nvs_read_str(h, KEY_WIFI_SSID,  config->wifi_ssid,       sizeof(config->wifi_ssid),       CONFIG_WIFI_SSID);
    nvs_read_str(h, KEY_WIFI_PASS,  config->wifi_password,   sizeof(config->wifi_password),   CONFIG_WIFI_PASSWORD);
    nvs_read_str(h, KEY_MQTT_URI,   config->mqtt_broker_uri, sizeof(config->mqtt_broker_uri), CONFIG_MQTT_BROKER_URI);
    nvs_read_str(h, KEY_MQTT_TOPIC, config->mqtt_topic,      sizeof(config->mqtt_topic),      CONFIG_MQTT_TOPIC);
    nvs_read_str(h, KEY_MQTT_USER,  config->mqtt_username,   sizeof(config->mqtt_username),   CONFIG_MQTT_USERNAME);
    nvs_read_str(h, KEY_MQTT_PASS,  config->mqtt_password,   sizeof(config->mqtt_password),   CONFIG_MQTT_PASSWORD);

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded from NVS: SSID=%s, Broker=%s, Topic=%s",
             config->wifi_ssid, config->mqtt_broker_uri, config->mqtt_topic);
    return ESP_OK;
}

esp_err_t config_save(const device_config_t *config)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, KEY_WIFI_SSID,  config->wifi_ssid);
    nvs_set_str(h, KEY_WIFI_PASS,  config->wifi_password);
    nvs_set_str(h, KEY_MQTT_URI,   config->mqtt_broker_uri);
    nvs_set_str(h, KEY_MQTT_TOPIC, config->mqtt_topic);
    nvs_set_str(h, KEY_MQTT_USER,  config->mqtt_username);
    nvs_set_str(h, KEY_MQTT_PASS,  config->mqtt_password);

    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Config saved to NVS: SSID=%s, Broker=%s, Topic=%s",
             config->wifi_ssid, config->mqtt_broker_uri, config->mqtt_topic);
    return err;
}

bool config_exists(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    size_t len = 0;
    err = nvs_get_str(h, KEY_WIFI_SSID, NULL, &len);
    nvs_close(h);

    return (err == ESP_OK && len > 1);
}

esp_err_t config_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Config erased from NVS");
    return err;
}
