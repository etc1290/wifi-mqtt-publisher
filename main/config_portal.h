/**
 * @file config_portal.h
 * @brief WiFi AP + Web Server configuration portal
 *
 * When WiFi or MQTT connection fails, this module starts the ESP32-C3
 * in AP mode with a web-based configuration page. Users connect to the
 * AP and open a browser to set WiFi / MQTT credentials.
 */

#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include "esp_err.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the configuration portal (AP mode + HTTP server).
 *
 * This function blocks until the user submits new settings via the
 * web form. After saving, it restarts the device automatically.
 *
 * @param current_config  Current config values to pre-fill the form
 * @param reason          Human-readable reason shown on the config page
 */
void config_portal_start(const device_config_t *current_config, const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_PORTAL_H */
