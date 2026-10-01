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
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "i2c_bsp.h"
#include "display_bsp.h"
#include "power_bsp.h"
#include "lvgl_bsp.h"
#include "lvgl.h"

#include "user_config.h"
#include "app_config.h"
#include "panel_store.h"
#include "panel_worker.h"
#include "provisioning.h"
#include "ui_panel.h"
#include "wifi_connect.h"

static const char *TAG = "herdr_panel";

I2cMasterBus user_i2cbus(BSP_I2C_SCL, BSP_I2C_SDA, BSP_I2C_NUM);
DisplayPort *user_display = NULL;

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

    /* AMOLED panel */
    user_display = new DisplayPort(user_i2cbus, BSP_LCD_H_RES, BSP_LCD_V_RES,
                                   BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1,
                                   BSP_LCD_DATA2, BSP_LCD_DATA3, BSP_LCD_CS,
                                   BSP_LCD_TOUCH_INT, BSP_LCD_TOUCH_RST);
    user_display->DisplayPort_TouchInit();
    user_display->Set_Backlight(CONFIG_HERDR_DEFAULT_BRIGHTNESS);

    /* LVGL port (display + touch) */
    Lvgl_PortInit(*user_display);

    /* config_service owns NVS; wifi_connect relies on that single init */
    ESP_ERROR_CHECK(app_config_init());
    panel_store_init();
    wifi_connect_start();       /* enters provisioning automatically when unconfigured */

    if (Lvgl_lock(-1) == ESP_OK) {
        ui_panel_init();
        if (provisioning_active()) {
            char ap_ssid[24];
            char ap_pass[12];
            char qr[128];
            provisioning_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
            provisioning_get_ap_pass(ap_pass, sizeof(ap_pass));
            provisioning_get_qr_payload(qr, sizeof(qr));
            ui_show_provisioning(ap_ssid, ap_pass, qr);
        }
        lv_timer_create(ui_tick_cb, 200, NULL);
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
