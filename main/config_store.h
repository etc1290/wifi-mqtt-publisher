/**
 * @file config_store.h
 * @brief NVS-backed configuration storage
 *
 * Stores WiFi and MQTT settings in NVS flash so they survive reboots.
 * Falls back to Kconfig defaults when NVS has no saved values.
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Device configuration (WiFi + MQTT)
 */
typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_broker_uri[129];
    char mqtt_topic[65];
    char mqtt_username[33];
    char mqtt_password[65];
} device_config_t;

/**
 * @brief Load config from NVS. Falls back to Kconfig defaults if empty.
 */
esp_err_t config_load(device_config_t *config);

/**
 * @brief Save config to NVS.
 */
esp_err_t config_save(const device_config_t *config);

/**
 * @brief Check if valid config exists in NVS.
 */
bool config_exists(void);

/**
 * @brief Erase saved config from NVS (revert to Kconfig defaults).
 */
esp_err_t config_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */
