#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *NVS_NS = "herdr";

static app_config_t s_cfg;
static SemaphoreHandle_t s_lock;
static bool s_nvs_ready;

static bool is_placeholder(const char *s)
{
    return s == NULL || s[0] == '\0' ||
           strcmp(s, "YOUR_SSID") == 0 ||
           strcmp(s, "YOUR_PASSWORD") == 0;
}

esp_err_t app_config_nvs_init(void)
{
    if (s_nvs_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    if (err == ESP_OK) s_nvs_ready = true;
    return err;
}

static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%.32s", CONFIG_HERDR_WIFI_SSID);
    snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%.64s", CONFIG_HERDR_WIFI_PASSWORD);
    snprintf(s_cfg.backend_host, sizeof(s_cfg.backend_host), "%.63s", CONFIG_HERDR_BACKEND_HOST);
    s_cfg.backend_port = CONFIG_HERDR_BACKEND_PORT;
    s_cfg.https = false;
}

esp_err_t app_config_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = app_config_nvs_init();
    if (err != ESP_OK) return err;

    load_defaults();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cfg.wifi_ssid);
        nvs_get_str(h, "ssid", s_cfg.wifi_ssid, &len);
        len = sizeof(s_cfg.wifi_pass);
        nvs_get_str(h, "pass", s_cfg.wifi_pass, &len);
        len = sizeof(s_cfg.backend_host);
        nvs_get_str(h, "host", s_cfg.backend_host, &len);
        nvs_get_u16(h, "port", &s_cfg.backend_port);
        len = sizeof(s_cfg.gateway_token);
        nvs_get_str(h, "token", s_cfg.gateway_token, &len);
        len = sizeof(s_cfg.continue_prompt);
        nvs_get_str(h, "contprompt", s_cfg.continue_prompt, &len);
        uint8_t https = 0;
        nvs_get_u8(h, "https", &https);
        s_cfg.https = https != 0;
        nvs_close(h);
    }
    return ESP_OK;
}

void app_config_get(app_config_t *out)
{
    if (out == NULL) return;
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

static esp_err_t validate(const app_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->wifi_ssid[0] == '\0' || is_placeholder(cfg->wifi_ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(cfg->wifi_ssid) > CFG_SSID_MAX) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->wifi_pass) > CFG_PASS_MAX) return ESP_ERR_INVALID_ARG;
    if (cfg->backend_host[0] == '\0' || strlen(cfg->backend_host) > CFG_HOST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->backend_port == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->gateway_token) > CFG_TOKEN_MAX) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->continue_prompt) > CFG_PROMPT_MAX) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    esp_err_t err = validate(cfg);
    if (err != ESP_OK) return err;
    if (!s_nvs_ready) {
        err = app_config_nvs_init();
        if (err != ESP_OK) return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, "ssid", cfg->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", cfg->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "host", cfg->backend_host);
    if (err == ESP_OK) err = nvs_set_u16(h, "port", cfg->backend_port);
    if (err == ESP_OK) err = nvs_set_str(h, "token", cfg->gateway_token);
    if (err == ESP_OK) err = nvs_set_str(h, "contprompt", cfg->continue_prompt);
    if (err == ESP_OK) err = nvs_set_u8(h, "https", cfg->https ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool app_config_is_configured(void)
{
    app_config_t c;
    app_config_get(&c);
    return c.wifi_ssid[0] != '\0' && !is_placeholder(c.wifi_ssid);
}

void app_config_base_url(char *buf, size_t buflen)
{
    app_config_t c;
    app_config_get(&c);
    snprintf(buf, buflen, "%s://%s:%u",
             c.https ? "https" : "http",
             c.backend_host, (unsigned)c.backend_port);
}

void app_config_auth_header(char *buf, size_t buflen)
{
    app_config_t c;
    app_config_get(&c);
    if (c.gateway_token[0] == '\0') {
        if (buflen > 0) buf[0] = '\0';
        return;
    }
    snprintf(buf, buflen, "Bearer %s", c.gateway_token);
}

esp_err_t app_config_clear_credentials(void)
{
    if (!s_nvs_ready) {
        esp_err_t err = app_config_nvs_init();
        if (err != ESP_OK) return err;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_erase_key(h, "token");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_request_edit(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "edit_pending", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool app_config_take_edit_request(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t requested = 0;
    esp_err_t err = nvs_get_u8(h, "edit_pending", &requested);
    if (err == ESP_OK && requested == 1) {
        err = nvs_erase_key(h, "edit_pending");
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK && requested == 1;
}
