#pragma once

/*
 * LVGL UI for the Herdr panel (UI_DESIGN.md).
 *
 * Screens: BOOT splash, HOM 4-grid, DET detail, CNF confirm, RST result, SET settings,
 *          PRV provisioning QR.
 *
 * Selection identity is terminal_id + selection_epoch (never array index).
 * All LVGL objects are touched only from the LVGL task / locked context.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Build all screens. Call with the LVGL lock held. */
void ui_panel_init(void);

/**
 * Refresh widgets from panel_store + drain panel_store UI events.
 * Called from a LVGL timer (lock held by adapter).
 */
void ui_panel_tick(void);

/** Show the provisioning (QR code) screen. */
void ui_show_provisioning(const char *ap_ssid, const char *ap_pass, const char *qr_payload);

/** Release splash after the boot target (home or provisioning) is known. */
void ui_panel_boot_ready(void);

#ifdef __cplusplus
}
#endif
