#pragma once

/* Runtime preferences. Only panel_worker writes them; UI and audio copy a
 * snapshot. Credentials stay in app_config and are never included here. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_PREF_SOUND_ENABLED = 0,
    PANEL_PREF_SOUND_VOLUME,
    PANEL_PREF_SOUND_REQUEST,
    PANEL_PREF_SOUND_DONE,
    PANEL_PREF_SOUND_SCOPE,
    PANEL_PREF_SOUND_CLAUDE,
    PANEL_PREF_SOUND_OPENCODE,
    PANEL_PREF_SOUND_PI,
    PANEL_PREF_QUIET_ENABLED,
    PANEL_PREF_QUIET_START,
    PANEL_PREF_QUIET_END,
    PANEL_PREF_UTC_OFFSET,
    PANEL_PREF_BRIGHTNESS,
    /* Standby is two-stage (UI_DESIGN.md §4.3): after idle_display_seconds
     * the panel shows the clock screen, and after idle_blank_seconds the
     * screen goes fully dark. Either stage can be disabled with 0. The two
     * fields must stay adjacent and before PANEL_PREF_DIM_BRIGHTNESS. */
    PANEL_PREF_IDLE_DISPLAY_SECONDS,
    PANEL_PREF_IDLE_BLANK_SECONDS,
    PANEL_PREF_MOTION_WAKE,
    PANEL_PREF_DIM_BRIGHTNESS,
    PANEL_PREF_REDUCE_MOTION,
    PANEL_PREF_VISUAL_ALERT,
    PANEL_PREF_OVERVIEW_INTERVAL,
    PANEL_PREF_CARD_ORDER,
    PANEL_PREF_HIDE_IDLE,
    PANEL_PREF_COUNT,
} panel_pref_field_t;

/* Standby stage durations in seconds. 0 disables that stage. */
enum {
    PANEL_STANDBY_OFF = 0,
    PANEL_STANDBY_1MIN = 60,
    PANEL_STANDBY_2MIN = 120,
    PANEL_STANDBY_3MIN = 180,
    PANEL_STANDBY_5MIN = 300,
    PANEL_STANDBY_10MIN = 600,
    PANEL_STANDBY_15MIN = 900,
};

enum { PANEL_SOUND_ALL = 0, PANEL_SOUND_PAGE = 1, PANEL_SOUND_SELECTED = 2 };
enum { PANEL_SOUND_INHERIT = 0, PANEL_SOUND_ON = 1, PANEL_SOUND_OFF = 2 };
enum { PANEL_ORDER_FIXED = 0, PANEL_ORDER_BLOCKED = 1, PANEL_ORDER_RECENT = 2 };

typedef struct {
    uint8_t sound_enabled;
    uint8_t sound_volume;
    uint8_t sound_request;
    uint8_t sound_done;
    uint8_t sound_scope;
    uint8_t sound_claude;
    uint8_t sound_opencode;
    uint8_t sound_pi;
    uint8_t quiet_enabled;
    uint16_t quiet_start;          /* minutes since midnight, 15-minute steps */
    uint16_t quiet_end;
    int16_t utc_offset_minutes;
    uint8_t brightness;
    uint16_t idle_display_seconds; /* clock screen after N s idle; 0 = never */
    uint16_t idle_blank_seconds;   /* fully dark after N s idle; 0 = never */
    uint8_t motion_wake;           /* accelerometer wakes the standby screen */
    uint8_t dim_brightness;
    uint8_t reduce_motion;
    uint8_t visual_alert;
    uint8_t overview_interval;     /* 2, 3, 5, 10 seconds */
    uint8_t card_order;
    uint8_t hide_idle;
} panel_prefs_t;

esp_err_t panel_prefs_init(void);
void panel_prefs_get(panel_prefs_t *out);
esp_err_t panel_prefs_set(panel_pref_field_t field, int value);
int panel_prefs_value(const panel_prefs_t *prefs, panel_pref_field_t field);

#ifdef __cplusplus
}
#endif
