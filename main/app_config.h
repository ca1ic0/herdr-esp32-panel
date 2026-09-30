#pragma once

/*
 * Persistent application configuration (WiFi + backend) stored in NVS.
 * Falls back to Kconfig defaults when NVS is empty.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char backend_host[64];
    uint16_t backend_port;
} app_config_t;

/** Initialise NVS and load the configuration. Call once at boot. */
void app_config_init(void);

/** Copy the current configuration (thread-safe). */
void app_config_get(app_config_t *out);

/** Persist a new configuration to NVS (thread-safe). */
esp_err_t app_config_save(const app_config_t *cfg);

/** True when a usable WiFi SSID has been configured. */
bool app_config_is_configured(void);

/** Build "http://host:port" from the current configuration. */
void app_config_base_url(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
