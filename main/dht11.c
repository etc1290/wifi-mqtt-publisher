/**
 * @file dht11.c
 * @brief DHT11 single-wire protocol implementation for ESP32-C3
 *
 * Protocol overview (40-bit transmission):
 *   1. MCU pulls data line LOW for ≥18 ms  → start signal
 *   2. MCU releases line, DHT11 pulls LOW ~80 µs then HIGH ~80 µs → ACK
 *   3. DHT11 sends 40 bits: each bit starts with ~50 µs LOW,
 *      then HIGH for ~26 µs (bit 0) or ~70 µs (bit 1)
 *   4. Data = [humidity_int][humidity_dec][temp_int][temp_dec][checksum]
 */

#include "dht11.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"             /* esp_timer_get_time – hardware µs timer */
#include "esp_rom_sys.h"           /* esp_rom_delay_us (works on RISC-V) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DHT11";

/* ---------- internal helpers ------------------------------------------- */

/**
 * @brief Wait until the GPIO reaches the expected level, or timeout.
 * @return Actual duration in µs (hardware timer), or -1 on timeout.
 *
 * Uses esp_timer_get_time() instead of a software counter so that
 * the returned value is real microseconds regardless of loop overhead.
 */
static int dht11_wait_for_level(gpio_num_t pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if ((esp_timer_get_time() - start) >= timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

/* ---------- public API ------------------------------------------------- */

esp_err_t dht11_read(gpio_num_t gpio_num, dht11_data_t *data)
{
    uint8_t bits[5] = {0};

    /* ── 1. Send start signal ──────────────────────────────────────────── */
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
    vTaskDelay(pdMS_TO_TICKS(20));          /* hold LOW ≥18 ms              */
    gpio_set_level(gpio_num, 1);
    esp_rom_delay_us(30);                   /* release for 20-40 µs         */

    /* ── 2. Switch to input & wait for DHT11 ACK ──────────────────────── */
    gpio_set_direction(gpio_num, GPIO_MODE_INPUT);

    /* DHT11 pulls LOW ~80 µs */
    if (dht11_wait_for_level(gpio_num, 0, 100) < 0) {
        ESP_LOGE(TAG, "No response from DHT11 (check wiring on GPIO %d)", gpio_num);
        return ESP_ERR_TIMEOUT;
    }
    if (dht11_wait_for_level(gpio_num, 1, 100) < 0) {
        ESP_LOGE(TAG, "ACK low phase timeout");
        return ESP_ERR_TIMEOUT;
    }
    /* DHT11 pulls HIGH ~80 µs */
    if (dht11_wait_for_level(gpio_num, 0, 100) < 0) {
        ESP_LOGE(TAG, "ACK high phase timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* ── 3. Read 40 data bits ─────────────────────────────────────────── */
    for (int i = 0; i < 40; i++) {
        /* Each bit starts with ~50 µs LOW */
        if (dht11_wait_for_level(gpio_num, 1, 100) < 0) {
            ESP_LOGE(TAG, "Bit %d: timeout waiting for HIGH", i);
            return ESP_ERR_TIMEOUT;
        }
        /* Measure HIGH duration to distinguish 0 vs 1 */
        int high_us = dht11_wait_for_level(gpio_num, 0, 100);
        if (high_us < 0) {
            ESP_LOGE(TAG, "Bit %d: timeout waiting for LOW", i);
            return ESP_ERR_TIMEOUT;
        }

        bits[i / 8] <<= 1;
        if (high_us > 40) {                 /* >40 µs HIGH ⇒ bit '1'       */
            bits[i / 8] |= 1;
        }
    }

    /* ── 4. Verify checksum ───────────────────────────────────────────── */
    uint8_t checksum = bits[0] + bits[1] + bits[2] + bits[3];
    if (checksum != bits[4]) {
        ESP_LOGE(TAG, "Checksum error: calculated 0x%02X, received 0x%02X",
                 checksum, bits[4]);
        return ESP_ERR_INVALID_CRC;
    }

    /* ── 5. Parse result ──────────────────────────────────────────────── */
    data->humidity    = (float)bits[0] + (float)bits[1] * 0.1f;
    data->temperature = (float)bits[2] + (float)bits[3] * 0.1f;

    ESP_LOGI(TAG, "Temperature: %.1f°C  Humidity: %.1f%%",
             data->temperature, data->humidity);
    return ESP_OK;
}
