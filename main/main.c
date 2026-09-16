/**
 * @file main.c
 * @brief ESP32-C3 WiFi → MQTT Publisher with DHT11 (Deep-Sleep Low-Power Mode)
 *
 * Flow (one-shot per wake cycle):
 *   1. Wake from deep sleep (or cold boot)
 *   2. LED ON → Initialize NVS, TCP/IP, event loop
 *   3. Connect to WiFi (STA mode, retries on failure)
 *   4. Connect to MQTT broker, wait for connection
 *   5. Read DHT11 sensor
 *   6. Publish JSON payload, wait for broker ACK (QoS 1)
 *   7. LED OFF → Enter deep sleep for N seconds
 *
 * Deep sleep current draw on ESP32-C3: ~5 µA (vs ~100 mA active with WiFi)
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
#include "esp_sleep.h"
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
#define DEEP_SLEEP_SEC          CONFIG_SENSOR_READ_INTERVAL_SEC

#define LED_GPIO                ((gpio_num_t)CONFIG_LED_GPIO)
#ifdef CONFIG_LED_ACTIVE_LOW
#define LED_ON_LEVEL  0
#define LED_OFF_LEVEL 1
#else
#define LED_ON_LEVEL  1
#define LED_OFF_LEVEL 0
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constants & globals
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *TAG = "MAIN";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MQTT_CONNECTED_BIT  BIT2
#define MQTT_PUBLISHED_BIT  BIT3

static EventGroupHandle_t s_event_group;
static int s_retry_count = 0;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* Boot count survives deep sleep (stored in RTC memory) */
static RTC_DATA_ATTR int s_boot_count = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  LED indicator helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, LED_OFF_LEVEL);
}

static void led_on(void)
{
    gpio_set_level(LED_GPIO, LED_ON_LEVEL);
}

static void led_off(void)
{
    gpio_set_level(LED_GPIO, LED_OFF_LEVEL);
}

/** Blink N times (for error indication) */
static void led_blink(int count, int on_ms, int off_ms)
{
    for (int i = 0; i < count; i++) {
        led_on();
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Deep sleep helper
 * ═══════════════════════════════════════════════════════════════════════ */

static void enter_deep_sleep(void)
{
    uint64_t sleep_us = (uint64_t)DEEP_SLEEP_SEC * 1000000ULL;

    ESP_LOGI(TAG, "💤 Entering deep sleep for %d seconds …", DEEP_SLEEP_SEC);
    led_off();

    /* Allow UART to flush logs before sleeping */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
    /* ── execution never reaches here ── */
}

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

static esp_err_t wifi_init_sta(void)
{
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

    /* Block until connected or failed (timeout 30s) */
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));

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
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "✓ MQTT publish ACK received (msg_id=%d)", event->msg_id);
        xEventGroupSetBits(s_event_group, MQTT_PUBLISHED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type: 0x%x", event->error_handle->error_type);
        break;

    default:
        break;
    }
}

static esp_err_t mqtt_connect(void)
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

    ESP_LOGI(TAG, "MQTT connecting to %s …", MQTT_BROKER_URI);

    /* Wait for MQTT connection (timeout 15s) */
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           MQTT_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & MQTT_CONNECTED_BIT) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "✗ MQTT connection timeout");
    return ESP_FAIL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Publish sensor data & wait for ACK
 * ═══════════════════════════════════════════════════════════════════════ */

static esp_err_t publish_sensor_data(void)
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

    /* DHT11 needs ~1 s settle time after power-on */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Read sensor (retry up to 3 times) */
    dht11_data_t sensor = {0};
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = dht11_read(DHT11_DATA_GPIO, &sensor);
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "DHT11 read attempt %d/3 failed (0x%x)", attempt, ret);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ DHT11 read failed after 3 attempts");
        return ret;
    }

    /* Build JSON payload */
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"temperature\":%.1f,\"humidity\":%.1f,\"boot\":%d}",
             sensor.temperature, sensor.humidity, s_boot_count);

    /* Publish with QoS 1 (at-least-once, broker sends PUBACK) */
    xEventGroupClearBits(s_event_group, MQTT_PUBLISHED_BIT);
    int msg_id = esp_mqtt_client_publish(
        s_mqtt_client, MQTT_TOPIC, payload, 0, /*qos=*/1, /*retain=*/0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "✗ MQTT publish call failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "📤 Publishing [%s] → %s  (msg_id=%d)",
             MQTT_TOPIC, payload, msg_id);

    /* Wait for broker PUBACK (timeout 10s) */
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           MQTT_PUBLISHED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));

    if (bits & MQTT_PUBLISHED_BIT) {
        ESP_LOGI(TAG, "✓ Data delivered to broker");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "⚠ PUBACK timeout – data may still arrive");
    return ESP_ERR_TIMEOUT;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point (runs once per wake cycle)
 * ═══════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    s_boot_count++;

    ESP_LOGI(TAG, "╔══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║ ESP32-C3 MQTT+DHT11 (Low Power Mode)    ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════╝");
    ESP_LOGI(TAG, "Boot #%d | Wake cause: %s",
             s_boot_count,
             (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)
                 ? "deep-sleep timer" : "power-on / reset");

    /* ── LED init & turn on (indicates active cycle) ──────────────────── */
    led_init();
    led_on();

    /* ── Event group for synchronization ──────────────────────────────── */
    s_event_group = xEventGroupCreate();

    /* ── Initialize NVS (required for WiFi) ───────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── Step 1: Connect WiFi ─────────────────────────────────────────── */
    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed → blink error → deep sleep");
        led_blink(5, 100, 100);                 /* fast blink = WiFi error */
        enter_deep_sleep();
    }

    /* ── Step 2: Connect MQTT ─────────────────────────────────────────── */
    if (mqtt_connect() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT failed → blink error → deep sleep");
        led_blink(3, 300, 300);                 /* slow blink = MQTT error */
        esp_mqtt_client_destroy(s_mqtt_client);
        enter_deep_sleep();
    }

    /* ── Step 3: Read sensor & publish ────────────────────────────────── */
    ret = publish_sensor_data();

    if (ret == ESP_OK) {
        /* Success: brief double-flash confirmation */
        led_off();
        vTaskDelay(pdMS_TO_TICKS(50));
        led_blink(2, 50, 50);
    } else {
        /* Sensor/publish error: triple slow blink */
        led_blink(3, 200, 200);
    }

    /* ── Step 4: Clean up & deep sleep ────────────────────────────────── */
    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    esp_wifi_stop();

    enter_deep_sleep();
}
