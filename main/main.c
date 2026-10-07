/**
 * @file main.c
 * @brief ESP32-C3 WiFi → MQTT Publisher with DHT11 sensor
 *
 * Flow:
 *   1. Initialize NVS → load config (NVS or Kconfig defaults)
 *   2. Try WiFi STA connection → if fail → config portal (AP mode)
 *   3. Try MQTT connection → if fail → config portal (AP mode)
 *   4. Every N seconds: read DHT11 → publish JSON to MQTT topic
 *
 * Config portal: AP "ESP32-Config" → http://192.168.4.1
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
#include "config_store.h"
#include "config_portal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Configuration
 * ═══════════════════════════════════════════════════════════════════════ */

#define DHT11_DATA_GPIO         ((gpio_num_t)CONFIG_DHT11_GPIO)
#define SENSOR_READ_INTERVAL    CONFIG_SENSOR_READ_INTERVAL_SEC
#define LED_GPIO                ((gpio_num_t)CONFIG_LED_GPIO)

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constants & globals
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *TAG = "MAIN";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MQTT_CONNECTED_BIT  BIT2

static EventGroupHandle_t s_event_group;
static int s_retry_count = 0;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

/* Active configuration (loaded from NVS or Kconfig defaults) */
static device_config_t s_config;

/* ═══════════════════════════════════════════════════════════════════════════
 *  LED indicator
 * ═══════════════════════════════════════════════════════════════════════ */

static void led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(LED_GPIO, 0);
}

static void led_set(bool on) { gpio_set_level(LED_GPIO, on ? 1 : 0); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  WiFi STA
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
            if (s_retry_count < CONFIG_WIFI_MAX_RETRY) {
                s_retry_count++;
                ESP_LOGW(TAG, "WiFi disconnected – retry %d/%d",
                         s_retry_count, CONFIG_WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "WiFi connect failed after %d retries",
                         CONFIG_WIFI_MAX_RETRY);
                xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✓ Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Try connecting to WiFi STA.
 * @return ESP_OK on success, ESP_FAIL if connection failed.
 */
static esp_err_t wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid,
            s_config.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            s_config.wifi_password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s …", s_config.wifi_ssid);

    /* Block until connected or failed (timeout 20s) */
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✓ WiFi connected to %s", s_config.wifi_ssid);
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
        ESP_LOGI(TAG, "✓ MQTT connected to %s", s_config.mqtt_broker_uri);
        s_mqtt_connected = true;
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
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

/**
 * @brief Start MQTT client and wait for connection.
 * @return ESP_OK on success, ESP_FAIL on timeout.
 */
static esp_err_t mqtt_connect(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_config.mqtt_broker_uri,
    };

    if (strlen(s_config.mqtt_username) > 0) {
        mqtt_cfg.credentials.username = s_config.mqtt_username;
    }
    if (strlen(s_config.mqtt_password) > 0) {
        mqtt_cfg.credentials.authentication.password = s_config.mqtt_password;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    ESP_LOGI(TAG, "MQTT connecting to %s …", s_config.mqtt_broker_uri);

    /* Wait for MQTT connection (timeout 15s) */
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           MQTT_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & MQTT_CONNECTED_BIT) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "✗ MQTT connection timeout");
    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    return ESP_FAIL;
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

    /* DHT11 warm-up: first read after power-on returns 0 */
    vTaskDelay(pdMS_TO_TICKS(2000));
    dht11_data_t dummy = {0};
    dht11_read(DHT11_DATA_GPIO, &dummy);
    ESP_LOGI(TAG, "DHT11 warm-up read done (discarded)");
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
                    s_mqtt_client, s_config.mqtt_topic,
                    payload, 0, /*qos=*/1, /*retain=*/0);
                ESP_LOGI(TAG, "📤 Published [%s] → %s  (msg_id=%d)",
                         s_config.mqtt_topic, payload, msg_id);

                /* Brief LED flash on successful publish */
                led_set(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                led_set(false);
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
 *  BOOT button monitor task (GPIO 9 – hold 3 s to factory-reset)
 * ═══════════════════════════════════════════════════════════════════════ */

#define BOOT_BUTTON_GPIO  GPIO_NUM_9

static void reset_button_task(void *pvParameters)
{
    /* Configure BOOT button GPIO */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    while (1) {
        /* Wait until button is pressed (active LOW) */
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            ESP_LOGW(TAG, "BOOT button pressed – hold 3 s to reset config …");

            /* Count how long button stays held, blink LED as feedback */
            int held_ms = 0;
            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && held_ms < 3000) {
                led_set((held_ms / 100) % 2 == 0);  /* fast blink */
                vTaskDelay(pdMS_TO_TICKS(50));
                held_ms += 50;
            }
            led_set(false);

            if (held_ms >= 3000) {
                ESP_LOGW(TAG, "🗑️  Erasing saved config from NVS …");
                config_erase();
                ESP_LOGW(TAG, "✓ Config erased – restarting …");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            } else {
                ESP_LOGI(TAG, "Button released early (%d ms) – ignored", held_ms);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));  /* poll every 100 ms */
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

    /* ── LED init ─────────────────────────────────────────────────────── */
    led_init();

    /* ── Initialize NVS ───────────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── Load config from NVS (or Kconfig defaults) ───────────────────── */
    config_load(&s_config);
    ESP_LOGI(TAG, "Config: SSID=%s  Broker=%s  Topic=%s",
             s_config.wifi_ssid, s_config.mqtt_broker_uri, s_config.mqtt_topic);

    /* ── Event group for sync ─────────────────────────────────────────── */
    s_event_group = xEventGroupCreate();

    /* ── Step 1: Connect WiFi ─────────────────────────────────────────── */
    led_set(true);  /* LED on while connecting */

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi failed → starting config portal …");
        led_set(false);
        config_portal_start(&s_config, "WiFi connection failed. Check SSID and password.");
        /* ↑ blocks until user saves, then restarts */
    }

    /* ── Step 2: Connect MQTT ─────────────────────────────────────────── */
    if (mqtt_connect() != ESP_OK) {
        ESP_LOGW(TAG, "MQTT failed → starting config portal …");
        led_set(false);
        config_portal_start(&s_config, "MQTT broker connection failed. Check broker URI.");
        /* ↑ blocks until user saves, then restarts */
    }

    led_set(false);

    /* ── Step 3: Launch background tasks ──────────────────────────────── */
    xTaskCreate(sensor_publish_task, "sensor_pub", 4096, NULL, 5, NULL);
    xTaskCreate(reset_button_task,   "reset_btn",  2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "🚀 System ready! Publishing every %d seconds to [%s]",
             SENSOR_READ_INTERVAL, s_config.mqtt_topic);
    ESP_LOGI(TAG, "💡 Hold BOOT button 3 s to reset config");
}
