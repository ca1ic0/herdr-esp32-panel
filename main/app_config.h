#pragma once

/*
 * config_service: the only NVS owner.
 *
 * Holds Wi-Fi credentials, gateway URL, and the device Bearer token.
 * Secrets are never logged or rendered into LVGL labels.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_SSID_MAX       32
#define CFG_PASS_MAX       64
#define CFG_HOST_MAX       63
#define CFG_TOKEN_MAX      95
#define CFG_PROMPT_MAX     160

typedef struct {
    char wifi_ssid[CFG_SSID_MAX + 1];
    char wifi_pass[CFG_PASS_MAX + 1];
    char backend_host[CFG_HOST_MAX + 1];
    uint16_t backend_port;
    char gateway_token[CFG_TOKEN_MAX + 1];   /* device-specific Bearer */
    char continue_prompt[CFG_PROMPT_MAX + 1]; /* empty = gateway default */
    bool https;                              /* reserved; v1 is LAN HTTP */
} app_config_t;

/** Initialise NVS once and load stored config. Call exactly once at boot. */
esp_err_t app_config_init(void);

/** Thread-safe copy of the current configuration. */
void app_config_get(app_config_t *out);

/**
 * Validate + persist. Checks field lengths and rejects empty SSID/host.
 * Returns ESP_ERR_INVALID_ARG on bad input, or NVS errors. Never reports
 * success when nvs_commit fails.
 */
esp_err_t app_config_save(const app_config_t *cfg);

/** True when a real (non-placeholder) SSID has been stored. */
bool app_config_is_configured(void);

/** Build "http://host:port" (or https) from the current configuration. */
void app_config_base_url(char *buf, size_t buflen);

/**
 * Build "Bearer <token>" or "" when no token is stored.
 * Used only by panel_api_client as an Authorization header.
 */
void app_config_auth_header(char *buf, size_t buflen);

/** Clear Wi-Fi + gateway credentials (re-provisioning). */
esp_err_t app_config_clear_credentials(void);

/* One-shot reboot marker for the five-minute connection editor. */
esp_err_t app_config_request_edit(void);
bool app_config_take_edit_request(void);

/** Single NVS flash init (used by app_config_init). */
esp_err_t app_config_nvs_init(void);

#ifdef __cplusplus
}
#endif
