#pragma once

/*
 * panel_store: single owner of cross-task panel snapshots.
 *
 * Producer: panel_worker (publishes after network parse).
 * Consumer: UI task (copies a read-only view under a short lock).
 *
 * Never hold the store lock while calling LVGL.
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "panel_model.h"
#include "panel_prefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- UI -> worker commands (bounded queues, non-blocking enqueue) ---- */

typedef enum {
    PANEL_CMD_OPEN_DETAIL = 1,
    PANEL_CMD_CLOSE_DETAIL,         /* user left the detail screen */
    PANEL_CMD_REFRESH,
    PANEL_CMD_RECONNECT,
    PANEL_CMD_SET_PREF,
    PANEL_CMD_EDIT_CONNECTION,
    PANEL_CMD_ACTION,               /* goes through action_q only */
} panel_cmd_type_t;

typedef struct {
    panel_cmd_type_t type;
    char terminal_id[PANEL_TERM_ID_LEN];
    uint32_t selection_epoch;
    /* ACTION payload */
    panel_action_id_t action;
    char context_token[PANEL_TOKEN_LEN];
    char prompt[PANEL_PROMPT_LEN];
    panel_pref_field_t pref_field;  /* SET_PREF */
    int pref_value;
    uint32_t edit_id;
} panel_cmd_t;

/* ---- worker -> UI one-shot events ----------------------------------- */

typedef enum {
    PANEL_EVT_ACTION_RESULT = 1,
    PANEL_EVT_CONFIG_RESULT,
    PANEL_EVT_FATAL_ERROR,
} panel_evt_type_t;

typedef struct {
    panel_evt_type_t type;
    panel_action_result_t action;
    panel_pref_field_t pref_field;
    uint32_t edit_id;
    bool config_saved;
    char message[PANEL_MESSAGE_LEN];
} panel_ui_evt_t;

/* ---- store API ------------------------------------------------------- */

void panel_store_init(void);

/** Publish a validated overview snapshot (copies under lock). */
void panel_store_publish_overview(const panel_overview_t *src);

/** Publish a validated detail snapshot (copies under lock). */
void panel_store_publish_detail(const panel_detail_t *src);

/** Update connectivity without touching agent cards. */
void panel_store_set_conn(panel_conn_state_t st, const char *message);
void panel_store_set_sound_notice(const char *message);
void panel_store_get_sound_notice(char *out, size_t capacity);

/** Copy current overview + connection for UI (lock held briefly). */
void panel_store_get_overview(panel_overview_t *out, panel_conn_state_t *conn_out);

/*
 * Copy the visible cards in display order (UI_DESIGN.md §4.1: the home
 * screen is a single scrolling list, so there is no page index any more).
 * `out` must have room for `max_out` entries (pass PANEL_MAX_AGENTS).
 * Returns the number of cards written. Copying only the visible cards keeps
 * the multi-KB snapshot off the LVGL task stack.
 */
int panel_store_get_visible(panel_agent_card_t *out, int max_out,
                            int *total_out, panel_conn_state_t *conn_out);

/** Copy current detail (lock held briefly). Returns false if none. */
bool panel_store_get_detail(panel_detail_t *out);

/** Monotonic generation counter; UI redraws when it changes. */
uint32_t panel_store_generation(void);

/** Current connectivity state. */
panel_conn_state_t panel_store_conn_state(void);

/** Index of the first blocked card in the stored overview, or -1. */
int panel_store_first_blocked_index(void);

/** Number of blocked cards in the stored overview. */
int panel_store_blocked_count(void);

/**
 * UI-owned current selection, used for sound scope filtering.
 * The home screen is a single scrolling list (UI_DESIGN.md §4.1), so there is
 * no page index any more: PANEL_SOUND_SELECTED matches the opened session and
 * PANEL_SOUND_PAGE matches everything currently visible in the list.
 */
void panel_store_set_view_context(const char *selected_terminal_id);
bool panel_store_sound_scope_match(const char *terminal_id, uint8_t scope);

/* ---- queues ---------------------------------------------------------- */

/** Capacity-1 action queue. Returns false if full or busy. */
bool panel_store_enqueue_action(const panel_cmd_t *cmd);

/** Capacity-4 control queue. Returns false if full. */
bool panel_store_enqueue_control(const panel_cmd_t *cmd);

/** Non-blocking drain helpers used by panel_worker. */
bool panel_store_recv_action(panel_cmd_t *out, TickType_t wait);
bool panel_store_recv_control(panel_cmd_t *out, TickType_t wait);

/** Capacity-8 UI event queue. Action results are never silently dropped. */
bool panel_store_post_ui_event(const panel_ui_evt_t *evt);
bool panel_store_recv_ui_event(panel_ui_evt_t *out, TickType_t wait);

/*
 * Single global outstanding action slot.
 * Returns true if the slot was free and is now reserved.
 * Released via panel_store_release_action().
 */
bool panel_store_try_reserve_action(void);
void panel_store_release_action(void);
bool panel_store_action_reserved(void);

#ifdef __cplusplus
}
#endif
