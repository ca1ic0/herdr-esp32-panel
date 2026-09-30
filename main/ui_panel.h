#pragma once

/*
 * LVGL UI for the Herdr panel.
 *
 * Home      : 2x2 grid of herdr session cards (swipe to page) + gear button.
 * Detail    : session content + ✓ / ✗ / → action buttons.
 * Settings  : WiFi / Backend sub-forms with on-screen keyboard.
 * Provision : QR code screen shown when the device is unconfigured.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Build all screens/widgets. Must be called with the LVGL lock held. */
void ui_panel_init(void);

/**
 * Refresh widgets from the shared state. Called periodically from a LVGL
 * timer while the LVGL lock is held by the timer's task context.
 */
void ui_panel_tick(void);

/** Show the provisioning (QR code) screen instead of the home grid. */
void ui_show_provisioning(const char *ap_ssid, const char *qr_payload);

#ifdef __cplusplus
}
#endif
