/**
 * @file main.c
 * @brief ESP32-C3 WiFi → MQTT Publisher with DHT11 sensor
 *
 * Flow:
 *   1. Initialize NVS, TCP/IP, event loop
 *   2. Connect to WiFi (STA mode, retries on failure)
 *   3. Connect to MQTT broker
 *   4. Every N seconds: read DHT11 → publish JSON to MQTT topic
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "dht11.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Configuration from Kconfig (idf.py menuconfig)
 * ═══════════════════════════════════════════════════════════════════════ */

#define WIFI_SSID               CONFIG_WIFI_SSID
#define WIFI_PASS               CONFIG_WIFI_PASSWORD
#define WIFI_MAX_RETRY          CONFIG_WIFI_MAX_RETRY

#define MQTT_BROKER_URI         CONFIG_MQTT_BROKER_URI
#define MQTT_TOPIC              CONFIG_MQTT_TOPIC
#define MQTT_USERNAME           CONFIG_MQTT_USERNAME
#define MQTT_PASSWORD           CONFIG_MQTT_PASSWORD

#define DHT11_DATA_GPIO         ((gpio_num_t)CONFIG_DHT11_GPIO)
#define SENSOR_READ_INTERVAL    CONFIG_SENSOR_READ_INTERVAL_SEC

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constants & globals
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *TAG = "MAIN";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

/* ═══════════════════════════════════════════════════════════════════════════
 *  WiFi
 * ═══════════════════════════════════════════════════════════════════════ */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started, connecting …");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_count < WIFI_MAX_RETRY) {
                s_retry_count++;
                ESP_LOGW(TAG, "WiFi disconnected – retry %d/%d",
                         s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "WiFi connect failed after %d retries", WIFI_MAX_RETRY);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✓ Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Configure WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid,     WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,  WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s …", WIFI_SSID);

    /* Block until connected or failed */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✓ WiFi connected to %s", WIFI_SSID);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "✗ WiFi connection failed");
    return ESP_FAIL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MQTT
 * ═══════════════════════════════════════════════════════════════════════ */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "✓ MQTT connected to %s", MQTT_BROKER_URI);
        s_mqtt_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected – will auto-reconnect");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type: 0x%x", event->error_handle->error_type);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT message published, msg_id=%d", event->msg_id);
        break;

    default:
        break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    /* Optional authentication */
    if (strlen(MQTT_USERNAME) > 0) {
        mqtt_cfg.credentials.username = MQTT_USERNAME;
    }
    if (strlen(MQTT_PASSWORD) > 0) {
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    ESP_LOGI(TAG, "MQTT client started → %s", MQTT_BROKER_URI);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sensor → MQTT publish task
 * ═══════════════════════════════════════════════════════════════════════ */

static void sensor_publish_task(void *pvParameters)
{
    /* Configure DHT11 GPIO with internal pull-up */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DHT11_DATA_GPIO),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Initial settle time for DHT11 (needs ~1 s after power-on) */
    vTaskDelay(pdMS_TO_TICKS(2000));

    char payload[128];
    int read_count = 0;

    while (1) {
        dht11_data_t sensor = {0};
        esp_err_t ret = dht11_read(DHT11_DATA_GPIO, &sensor);

        if (ret == ESP_OK) {
            read_count++;
            snprintf(payload, sizeof(payload),
                     "{\"temperature\":%.1f,\"humidity\":%.1f,\"count\":%d}",
                     sensor.temperature, sensor.humidity, read_count);

            if (s_mqtt_connected) {
                int msg_id = esp_mqtt_client_publish(
                    s_mqtt_client, MQTT_TOPIC, payload, 0, /*qos=*/1, /*retain=*/0);
                ESP_LOGI(TAG, "📤 Published [%s] → %s  (msg_id=%d)",
                         MQTT_TOPIC, payload, msg_id);
            } else {
                ESP_LOGW(TAG, "MQTT not connected – skipping publish");
            }
        } else {
            ESP_LOGW(TAG, "DHT11 read failed (err=0x%x) – retrying next cycle", ret);
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL * 1000));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   ESP32-C3 WiFi MQTT DHT11 Publisher    ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════╝");

    /* ── Initialize NVS (required for WiFi) ───────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── Connect to WiFi ──────────────────────────────────────────────── */
    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed – restarting in 5 seconds …");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* ── Start MQTT client ────────────────────────────────────────────── */
    mqtt_init();

    /* ── Launch sensor reading + publishing task ──────────────────────── */
    xTaskCreate(sensor_publish_task, "sensor_pub", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "🚀 System ready! Publishing every %d seconds to [%s]",
             SENSOR_READ_INTERVAL, MQTT_TOPIC);
}
