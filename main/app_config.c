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
static bool s_loaded;

static void load_defaults(void)
{
    snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%.32s", CONFIG_HERDR_WIFI_SSID);
    snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%.64s", CONFIG_HERDR_WIFI_PASSWORD);
    snprintf(s_cfg.backend_host, sizeof(s_cfg.backend_host), "%.63s", CONFIG_HERDR_BACKEND_HOST);
    s_cfg.backend_port = CONFIG_HERDR_BACKEND_PORT;
}

void app_config_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    (void)err;

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
        nvs_close(h);
    }
    s_loaded = true;
}

void app_config_get(app_config_t *out)
{
    if (out == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(h, "ssid", cfg->wifi_ssid);
    nvs_set_str(h, "pass", cfg->wifi_pass);
    nvs_set_str(h, "host", cfg->backend_host);
    nvs_set_u16(h, "port", cfg->backend_port);
    nvs_commit(h);
    nvs_close(h);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool app_config_is_configured(void)
{
    app_config_t c;
    app_config_get(&c);
    return c.wifi_ssid[0] != '\0' && strcmp(c.wifi_ssid, "YOUR_SSID") != 0;
}

void app_config_base_url(char *buf, size_t buflen)
{
    app_config_t c;
    app_config_get(&c);
    snprintf(buf, buflen, "http://%s:%u", c.backend_host, (unsigned)c.backend_port);
}
