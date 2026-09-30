/*
 * Herdr Panel firmware for Waveshare ESP32-C6-Touch-AMOLED-2.16.
 *
 * The board is a pure frontend for the herdr-restful backend running on a
 * PC: it polls session state over HTTP and sends allow/deny/continue
 * keystrokes back to the selected session.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
#include "herdr_client.h"
#include "panel_state.h"
#include "provisioning.h"
#include "ui_panel.h"
#include "wifi_connect.h"

static const char *TAG = "herdr_panel";

I2cMasterBus user_i2cbus(BSP_I2C_SCL, BSP_I2C_SDA, BSP_I2C_NUM);
DisplayPort *user_display = NULL;

/* ------------------------------------------------------------------ */
/* network worker: polls sessions and serves queued UI requests        */
/* ------------------------------------------------------------------ */

static void update_conn_label(const char *text)
{
    if (strcmp(g_conn, text) != 0) {
        snprintf(g_conn, sizeof(g_conn), "%s", text);
        g_conn_dirty = true;
    }
}

static void net_task(void *arg)
{
    (void)arg;
    herdr_snapshot_t snap;
    panel_msg_t msg;
    TickType_t last_poll = 0;
    char conn[64];

    for (;;) {
        /* 1. serve queued UI requests first (low latency) */
        while (xQueueReceive(g_msg_queue, &msg, 0) == pdTRUE) {
            if (msg.type == PANEL_MSG_SEND_TEXT) {
                esp_err_t err = herdr_send_text(msg.pane_id, msg.text, true);
                panel_set_toast(err == ESP_OK ? "sent" : "send failed");
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "send_text failed: %s", esp_err_to_name(err));
                }
            } else if (msg.type == PANEL_MSG_FETCH_OUTPUT) {
                if (herdr_fetch_output(msg.pane_id, g_output_buf,
                                       sizeof(g_output_buf), 12) == ESP_OK) {
                    g_output_dirty = true;
                }
            }
        }

        /* 2. poll the session list */
        if (!wifi_is_connected()) {
            update_conn_label("wifi connecting...");
        } else {
            TickType_t now = xTaskGetTickCount();
            if (now - last_poll >= pdMS_TO_TICKS(CONFIG_HERDR_POLL_INTERVAL_MS)) {
                last_poll = now;

                char ip[16];
                wifi_get_ip(ip, sizeof(ip));

                if (herdr_fetch_sessions(&snap) == ESP_OK) {
                    if (xSemaphoreTake(g_snap_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
                        g_snap = snap;
                        xSemaphoreGive(g_snap_lock);
                        g_snap_dirty = true;
                    }
                    snprintf(conn, sizeof(conn), "%s | %d session%s",
                             ip, snap.count, snap.count == 1 ? "" : "s");
                } else {
                    snprintf(conn, sizeof(conn), "%s | backend error", ip);
                }
                update_conn_label(conn);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
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

    /* AMOLED panel: SH8601 over QSPI */
    user_display = new DisplayPort(user_i2cbus, BSP_LCD_H_RES, BSP_LCD_V_RES,
                                   BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1,
                                   BSP_LCD_DATA2, BSP_LCD_DATA3, BSP_LCD_CS,
                                   BSP_LCD_TOUCH_INT, BSP_LCD_TOUCH_RST);
    user_display->DisplayPort_TouchInit();
    user_display->Set_Backlight(80);

    /* LVGL port (display + touch) */
    Lvgl_PortInit(*user_display);

    panel_state_init();
    wifi_connect_start();       /* enters provisioning automatically when unconfigured */

    if (Lvgl_lock(-1) == ESP_OK) {
        ui_panel_init();
        if (provisioning_active()) {
            char ap_ssid[24];
            char qr[96];
            provisioning_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
            provisioning_get_qr_payload(qr, sizeof(qr));
            ui_show_provisioning(ap_ssid, qr);
        }
        lv_timer_create(ui_tick_cb, 200, NULL);
        Lvgl_unlock();
    }

    if (!provisioning_active()) {
        xTaskCreatePinnedToCore(net_task, "herdr_net", 12 * 1024, NULL, 5, NULL, 0);
        char base[96];
        app_config_base_url(base, sizeof(base));
        ESP_LOGI(TAG, "Herdr Panel up (backend: %s)", base);
    } else {
        ESP_LOGI(TAG, "Herdr Panel in provisioning mode");
    }
}
