/*
 * Herdr Panel firmware for Waveshare ESP32-C6-Touch-AMOLED-2.16.
 *
 * Lifecycle only: board bring-up, config, Wi-Fi/provisioning, start
 * panel_worker + LVGL UI. No HTTP and no LVGL outside their owners.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "i2c_bsp.h"
#include "display_bsp.h"
#include "power_bsp.h"
#include "lvgl_bsp.h"
#include "lvgl.h"

#include "user_config.h"
#include "app_config.h"
#include "panel_display.h"
#include "panel_audio.h"
#include "panel_power.h"
#include "panel_prefs.h"
#include "panel_store.h"
#include "panel_worker.h"
#include "provisioning.h"
#include "ui_panel.h"
#include "wifi_connect.h"

static const char *TAG = "herdr_panel";

I2cMasterBus user_i2cbus(BSP_I2C_SCL, BSP_I2C_SDA, BSP_I2C_NUM);
DisplayPort *user_display = NULL;
static portMUX_TYPE s_power_lock = portMUX_INITIALIZER_UNLOCKED;
static panel_power_status_t s_power_status = { false, false, false, -1, 0 };
static bool s_power_valid;

extern "C" bool panel_power_get(panel_power_status_t *out)
{
    if (out == nullptr) return false;
    portENTER_CRITICAL(&s_power_lock);
    *out = s_power_status;
    bool valid = s_power_valid;
    portEXIT_CRITICAL(&s_power_lock);
    return valid;
}

static void power_sample_task(void *arg)
{
    (void)arg;
    for (;;) {
        PmicBatteryStatus sample;
        if (Custom_PmicReadBattery(&sample)) {
            portENTER_CRITICAL(&s_power_lock);
            s_power_status.battery_present = sample.battery_present;
            s_power_status.external_power = sample.external_power;
            s_power_status.charging = sample.charging;
            s_power_status.percent = sample.percent;
            s_power_status.millivolts = sample.millivolts;
            s_power_valid = true;
            portEXIT_CRITICAL(&s_power_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

extern "C" void panel_display_set_brightness(uint8_t percent)
{
    if (user_display != NULL) user_display->Set_Backlight(percent);
}

/* ------------------------------------------------------------------ */
/* LVGL periodic refresh (runs inside the LVGL task)                   */
/* ------------------------------------------------------------------ */

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_panel_tick();
}

/* ------------------------------------------------------------------ */

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Herdr Panel starting");

    /* power management chip (AXP2101) — must be first: it switches the
     * display rail (ALDO3) used by the panel reset sequence */
    Custom_PmicPortInit(&user_i2cbus, 0x34);

    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(panel_prefs_init());
    panel_prefs_t prefs;
    panel_prefs_get(&prefs);

    /* AMOLED panel */
    user_display = new DisplayPort(user_i2cbus, BSP_LCD_H_RES, BSP_LCD_V_RES,
                                   BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1,
                                   BSP_LCD_DATA2, BSP_LCD_DATA3, BSP_LCD_CS,
                                   BSP_LCD_TOUCH_INT, BSP_LCD_TOUCH_RST);
    user_display->DisplayPort_TouchInit();
    panel_display_set_brightness(prefs.brightness);

    /* LVGL port (display + touch) */
    Lvgl_PortInit(*user_display);
    xTaskCreatePinnedToCore(power_sample_task, "power_sample", 3072,
                            NULL, 3, NULL, 0);

    /* Bring up the splash before network and codec startup so it animates
     * while those subsystems initialise. LVGL remains owned by its task. */
    panel_store_init();
    if (Lvgl_lock(-1) == ESP_OK) {
        ui_panel_init();
        lv_timer_create(ui_tick_cb, 200, NULL);
        Lvgl_unlock();
    }

    if (!panel_audio_start(user_i2cbus.Get_I2cBusHandle())) {
        ESP_LOGW(TAG, "audio task unavailable; visual alerts remain active");
    }
    wifi_connect_start();       /* enters provisioning automatically when unconfigured */
    if (!provisioning_active()) {
        esp_sntp_config_t time_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t time_err = esp_netif_sntp_init(&time_cfg);
        if (time_err != ESP_OK) ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(time_err));
    }

    if (Lvgl_lock(-1) == ESP_OK) {
        if (provisioning_active()) {
            char ap_ssid[24];
            char ap_pass[12];
            char qr[128];
            provisioning_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
            provisioning_get_ap_pass(ap_pass, sizeof(ap_pass));
            provisioning_get_qr_payload(qr, sizeof(qr));
            ui_show_provisioning(ap_ssid, ap_pass, qr);
        }
        ui_panel_boot_ready();
        Lvgl_unlock();
    }

    if (!provisioning_active()) {
        panel_worker_start();
        char base[96];
        app_config_base_url(base, sizeof(base));
        ESP_LOGI(TAG, "Herdr Panel up (gateway: %s)", base);
    } else {
        ESP_LOGI(TAG, "Herdr Panel in provisioning mode");
    }
}
