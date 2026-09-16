/**
 * @file dht11.h
 * @brief DHT11 Temperature & Humidity Sensor Driver for ESP32-C3
 *
 * Single-wire protocol driver. Requires a 4.7kΩ pull-up resistor
 * on the data line (or enable internal pull-up via GPIO config).
 */

#ifndef DHT11_H
#define DHT11_H

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DHT11 sensor reading result
 */
typedef struct {
    float temperature;  /*!< Temperature in °C (0–50) */
    float humidity;     /*!< Relative humidity in % (20–90) */
} dht11_data_t;

/**
 * @brief Read temperature and humidity from a DHT11 sensor
 *
 * @param gpio_num  GPIO pin connected to DHT11 data pin
 * @param[out] data Pointer to store the reading result
 * @return
 *      - ESP_OK            Success
 *      - ESP_ERR_TIMEOUT   Communication timeout (check wiring)
 *      - ESP_ERR_INVALID_CRC  Checksum mismatch (noisy signal)
 */
esp_err_t dht11_read(gpio_num_t gpio_num, dht11_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* DHT11_H */
