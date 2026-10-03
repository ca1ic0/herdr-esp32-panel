#include "panel_prefs.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* v2 replaced idle_dim_seconds with the two-stage standby pair plus
 * motion_wake; the blob layout changed, so old blobs must be discarded. */
#define PREFS_SCHEMA 2u
#define PREFS_NAMESPACE "panelprefs"
#define PREFS_KEY "v1"

typedef struct {
    uint32_t schema;
    panel_prefs_t prefs;
} prefs_blob_t;

static SemaphoreHandle_t s_lock;
static panel_prefs_t s_prefs;

static panel_prefs_t defaults(void)
{
    panel_prefs_t p = {
        .sound_enabled = 1, .sound_volume = 50,
        .sound_request = 1, .sound_done = 1,
        .sound_scope = PANEL_SOUND_ALL,
        .sound_claude = PANEL_SOUND_INHERIT,
        .sound_opencode = PANEL_SOUND_INHERIT,
        .sound_pi = PANEL_SOUND_INHERIT,
        .quiet_enabled = 0, .quiet_start = 22 * 60, .quiet_end = 8 * 60,
        .utc_offset_minutes = 480,
        .brightness = 45,
        .idle_display_seconds = PANEL_STANDBY_3MIN,
        .idle_blank_seconds = PANEL_STANDBY_5MIN,
        .motion_wake = 1,
        .dim_brightness = 15,
        .reduce_motion = 0, .visual_alert = 1,
        .overview_interval = 3, .card_order = PANEL_ORDER_FIXED,
        .hide_idle = 0,
    };
    return p;
}

static const uint16_t k_standby_stages[] = {
    PANEL_STANDBY_OFF, PANEL_STANDBY_1MIN, PANEL_STANDBY_2MIN,
    PANEL_STANDBY_3MIN, PANEL_STANDBY_5MIN, PANEL_STANDBY_10MIN,
    PANEL_STANDBY_15MIN,
};
#define STAGE_COUNT (sizeof(k_standby_stages) / sizeof(k_standby_stages[0]))

/* Step to the first stage strictly greater than v, clamped to the last one. */
static uint16_t next_standby_stage(uint16_t v)
{
    for (size_t i = 0; i < STAGE_COUNT; i++) {
        if (k_standby_stages[i] > v) return k_standby_stages[i];
    }
    return k_standby_stages[STAGE_COUNT - 1];
}

/* Step to the last stage strictly smaller than v, clamped to the first. */
static uint16_t previous_standby_stage(uint16_t v)
{
    uint16_t out = k_standby_stages[0];
    for (size_t i = 0; i < STAGE_COUNT; i++) {
        if (k_standby_stages[i] < v) out = k_standby_stages[i];
        else break;
    }
    return out;
}

static bool valid(panel_pref_field_t field, int v)
{
    switch (field) {
    case PANEL_PREF_SOUND_ENABLED: case PANEL_PREF_SOUND_REQUEST:
    case PANEL_PREF_SOUND_DONE: case PANEL_PREF_QUIET_ENABLED:
    case PANEL_PREF_REDUCE_MOTION: case PANEL_PREF_VISUAL_ALERT:
    case PANEL_PREF_HIDE_IDLE: case PANEL_PREF_MOTION_WAKE:
        return v == 0 || v == 1;
    case PANEL_PREF_SOUND_VOLUME: return v >= 0 && v <= 100 && v % 10 == 0;
    case PANEL_PREF_SOUND_SCOPE: case PANEL_PREF_SOUND_CLAUDE:
    case PANEL_PREF_SOUND_OPENCODE: case PANEL_PREF_SOUND_PI:
    case PANEL_PREF_CARD_ORDER: return v >= 0 && v <= 2;
    case PANEL_PREF_QUIET_START: case PANEL_PREF_QUIET_END:
        return v >= 0 && v < 1440 && v % 15 == 0;
    case PANEL_PREF_UTC_OFFSET:
        return v >= -720 && v <= 840 && v % 15 == 0;
    case PANEL_PREF_BRIGHTNESS: return v >= 10 && v <= 100 && v % 5 == 0;
    case PANEL_PREF_IDLE_DISPLAY_SECONDS:
    case PANEL_PREF_IDLE_BLANK_SECONDS:
        return v == PANEL_STANDBY_OFF || v == PANEL_STANDBY_1MIN ||
               v == PANEL_STANDBY_2MIN || v == PANEL_STANDBY_3MIN ||
               v == PANEL_STANDBY_5MIN || v == PANEL_STANDBY_10MIN ||
               v == PANEL_STANDBY_15MIN;
    case PANEL_PREF_DIM_BRIGHTNESS: return v >= 5 && v <= 50 && v % 5 == 0;
    case PANEL_PREF_OVERVIEW_INTERVAL:
        return v == 2 || v == 3 || v == 5 || v == 10;
    default: return false;
    }
}

int panel_prefs_value(const panel_prefs_t *p, panel_pref_field_t field)
{
    if (p == NULL) return -1;
    switch (field) {
    case PANEL_PREF_SOUND_ENABLED: return p->sound_enabled;
    case PANEL_PREF_SOUND_VOLUME: return p->sound_volume;
    case PANEL_PREF_SOUND_REQUEST: return p->sound_request;
    case PANEL_PREF_SOUND_DONE: return p->sound_done;
    case PANEL_PREF_SOUND_SCOPE: return p->sound_scope;
    case PANEL_PREF_SOUND_CLAUDE: return p->sound_claude;
    case PANEL_PREF_SOUND_OPENCODE: return p->sound_opencode;
    case PANEL_PREF_SOUND_PI: return p->sound_pi;
    case PANEL_PREF_QUIET_ENABLED: return p->quiet_enabled;
    case PANEL_PREF_QUIET_START: return p->quiet_start;
    case PANEL_PREF_QUIET_END: return p->quiet_end;
    case PANEL_PREF_UTC_OFFSET: return p->utc_offset_minutes;
    case PANEL_PREF_BRIGHTNESS: return p->brightness;
    case PANEL_PREF_IDLE_DISPLAY_SECONDS: return p->idle_display_seconds;
    case PANEL_PREF_IDLE_BLANK_SECONDS: return p->idle_blank_seconds;
    case PANEL_PREF_MOTION_WAKE: return p->motion_wake;
    case PANEL_PREF_DIM_BRIGHTNESS: return p->dim_brightness;
    case PANEL_PREF_REDUCE_MOTION: return p->reduce_motion;
    case PANEL_PREF_VISUAL_ALERT: return p->visual_alert;
    case PANEL_PREF_OVERVIEW_INTERVAL: return p->overview_interval;
    case PANEL_PREF_CARD_ORDER: return p->card_order;
    case PANEL_PREF_HIDE_IDLE: return p->hide_idle;
    default: return -1;
    }
}

static bool valid_all(const panel_prefs_t *p)
{
    for (int i = 0; i < PANEL_PREF_COUNT; i++) {
        if (!valid((panel_pref_field_t)i,
                   panel_prefs_value(p, (panel_pref_field_t)i))) return false;
    }
    if (p->dim_brightness > p->brightness) return false;
    /* A blank threshold at or before the clock threshold would make the
     * clock screen unreachable, so require a strict ordering. */
    if (p->idle_blank_seconds != 0 && p->idle_display_seconds != 0 &&
        p->idle_blank_seconds <= p->idle_display_seconds) return false;
    return true;
}

esp_err_t panel_prefs_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }
    s_prefs = defaults();
    nvs_handle_t h;
    esp_err_t err = nvs_open(PREFS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    prefs_blob_t blob = { 0 };
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, PREFS_KEY, &blob, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH)
        return ESP_OK;
    if (err != ESP_OK) return err;
    if (len == sizeof(blob) && blob.schema == PREFS_SCHEMA &&
        valid_all(&blob.prefs)) s_prefs = blob.prefs;
    return ESP_OK;
}

void panel_prefs_get(panel_prefs_t *out)
{
    if (out == NULL) return;
    if (s_lock == NULL) {
        *out = defaults();
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_prefs;
    xSemaphoreGive(s_lock);
}

esp_err_t panel_prefs_set(panel_pref_field_t field, int value)
{
    if (!valid(field, value) || s_lock == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    panel_prefs_t p = s_prefs;
    switch (field) {
    case PANEL_PREF_SOUND_ENABLED: p.sound_enabled = value; break;
    case PANEL_PREF_SOUND_VOLUME: p.sound_volume = value; break;
    case PANEL_PREF_SOUND_REQUEST: p.sound_request = value; break;
    case PANEL_PREF_SOUND_DONE: p.sound_done = value; break;
    case PANEL_PREF_SOUND_SCOPE: p.sound_scope = value; break;
    case PANEL_PREF_SOUND_CLAUDE: p.sound_claude = value; break;
    case PANEL_PREF_SOUND_OPENCODE: p.sound_opencode = value; break;
    case PANEL_PREF_SOUND_PI: p.sound_pi = value; break;
    case PANEL_PREF_QUIET_ENABLED: p.quiet_enabled = value; break;
    case PANEL_PREF_QUIET_START: p.quiet_start = value; break;
    case PANEL_PREF_QUIET_END: p.quiet_end = value; break;
    case PANEL_PREF_UTC_OFFSET: p.utc_offset_minutes = value; break;
    case PANEL_PREF_BRIGHTNESS: p.brightness = value; break;
    case PANEL_PREF_IDLE_DISPLAY_SECONDS: p.idle_display_seconds = value; break;
    case PANEL_PREF_IDLE_BLANK_SECONDS: p.idle_blank_seconds = value; break;
    case PANEL_PREF_MOTION_WAKE: p.motion_wake = value; break;
    case PANEL_PREF_DIM_BRIGHTNESS: p.dim_brightness = value; break;
    case PANEL_PREF_REDUCE_MOTION: p.reduce_motion = value; break;
    case PANEL_PREF_VISUAL_ALERT: p.visual_alert = value; break;
    case PANEL_PREF_OVERVIEW_INTERVAL: p.overview_interval = value; break;
    case PANEL_PREF_CARD_ORDER: p.card_order = value; break;
    case PANEL_PREF_HIDE_IDLE: p.hide_idle = value; break;
    default: xSemaphoreGive(s_lock); return ESP_ERR_INVALID_ARG;
    }
    if (p.dim_brightness > p.brightness) {
        /* Keep a lowered normal brightness usable without a second edit. */
        p.dim_brightness = p.brightness;
    }
    if (p.idle_blank_seconds != 0 && p.idle_display_seconds != 0 &&
        p.idle_blank_seconds <= p.idle_display_seconds) {
        /* The user only edits one field at a time, so push the other one out
         * of the way instead of rejecting the write and silently doing
         * nothing. Lowering the clock past the blank threshold pushes blank
         * one step further; raising blank above clock pulls blank back. */
        if (field == PANEL_PREF_IDLE_DISPLAY_SECONDS) {
            p.idle_blank_seconds = next_standby_stage(p.idle_display_seconds);
        } else {
            p.idle_display_seconds = previous_standby_stage(p.idle_blank_seconds);
        }
    }
    prefs_blob_t blob = { .schema = PREFS_SCHEMA, .prefs = p };
    nvs_handle_t h;
    esp_err_t err = nvs_open(PREFS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, PREFS_KEY, &blob, sizeof(blob));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) s_prefs = p;
    xSemaphoreGive(s_lock);
    return err;
}
