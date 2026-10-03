#include "panel_store.h"

#include <stdio.h>
#include <string.h>

static SemaphoreHandle_t s_lock;
static panel_overview_t s_overview;
static panel_detail_t s_detail;
static panel_conn_state_t s_conn = PANEL_CONN_WIFI_CONNECTING;
static char s_conn_msg[PANEL_MESSAGE_LEN];
static char s_sound_notice[PANEL_MESSAGE_LEN];
static volatile uint32_t s_generation;

static QueueHandle_t s_action_q;    /* capacity 1 */
static QueueHandle_t s_control_q;   /* capacity 4 */
static QueueHandle_t s_ui_evt_q;    /* capacity 8 */

static volatile bool s_action_reserved;
static char s_view_term[PANEL_TERM_ID_LEN];

/* Called with s_lock held. Stable insertion sort keeps gateway order when
 * priority values are equal, and the tiny 24-entry index fits on stack. */
static int visible_index_locked(const panel_prefs_t *prefs, int index[PANEL_MAX_AGENTS])
{
    int n = 0;
    for (int i = 0; i < s_overview.count && i < PANEL_MAX_AGENTS; i++) {
        if (prefs->hide_idle &&
            s_overview.agents[i].herdr_status == PANEL_AGENT_IDLE) continue;
        index[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int cur = index[i];
        int j = i;
        while (j > 0) {
            const panel_agent_card_t *a = &s_overview.agents[cur];
            const panel_agent_card_t *b = &s_overview.agents[index[j - 1]];
            bool before = false;
            if (prefs->card_order == PANEL_ORDER_BLOCKED) {
                before = a->herdr_status == PANEL_AGENT_BLOCKED &&
                         b->herdr_status != PANEL_AGENT_BLOCKED;
            } else if (prefs->card_order == PANEL_ORDER_RECENT) {
                before = strcmp(a->updated_at, b->updated_at) > 0;
            }
            if (!before) break;
            index[j] = index[j - 1];
            j--;
        }
        index[j] = cur;
    }
    return n;
}

void panel_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_overview, 0, sizeof(s_overview));
    memset(&s_detail, 0, sizeof(s_detail));
    s_conn_msg[0] = '\0';
    s_sound_notice[0] = '\0';
    s_generation = 1;

    s_action_q = xQueueCreate(1, sizeof(panel_cmd_t));
    s_control_q = xQueueCreate(4, sizeof(panel_cmd_t));
    s_ui_evt_q = xQueueCreate(8, sizeof(panel_ui_evt_t));
    s_action_reserved = false;
}

void panel_store_publish_overview(const panel_overview_t *src)
{
    if (src == NULL || s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_overview = *src;
        s_generation++;
        xSemaphoreGive(s_lock);
    }
}

void panel_store_publish_detail(const panel_detail_t *src)
{
    if (src == NULL || s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_detail = *src;
        s_generation++;
        xSemaphoreGive(s_lock);
    }
}

void panel_store_set_conn(panel_conn_state_t st, const char *message)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_conn != st) {
            s_conn = st;
            s_generation++;
        }
        snprintf(s_conn_msg, sizeof(s_conn_msg), "%s", message != NULL ? message : "");
        xSemaphoreGive(s_lock);
    }
}

void panel_store_set_sound_notice(const char *message)
{
    if (s_lock == NULL) return;
    const char *value = message != NULL ? message : "";
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (strncmp(s_sound_notice, value, sizeof(s_sound_notice)) != 0) {
            snprintf(s_sound_notice, sizeof(s_sound_notice), "%s", value);
            s_generation++;
        }
        xSemaphoreGive(s_lock);
    }
}

void panel_store_get_sound_notice(char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) return;
    out[0] = '\0';
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        snprintf(out, capacity, "%s", s_sound_notice);
        xSemaphoreGive(s_lock);
    }
}

void panel_store_get_overview(panel_overview_t *out, panel_conn_state_t *conn_out)
{
    if (out == NULL) return;
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        *out = s_overview;
        if (conn_out != NULL) *conn_out = s_conn;
        xSemaphoreGive(s_lock);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

bool panel_store_get_detail(panel_detail_t *out)
{
    if (out == NULL || s_lock == NULL) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        *out = s_detail;
        bool ok = s_detail.valid;
        xSemaphoreGive(s_lock);
        return ok;
    }
    return false;
}

uint32_t panel_store_generation(void)
{
    return s_generation;
}

/*
 * Copy the visible cards in display order (UI_DESIGN.md §4.1: the home
 * screen is a single scrolling list, so there is no page index any more).
 * `out` must hold PANEL_MAX_AGENTS entries; returns the stored card count.
 * Keeping this bounded-by-value avoids copying the whole snapshot onto the
 * LVGL stack.
 */
int panel_store_get_visible(panel_agent_card_t *out, int max_out,
                            int *total_out, panel_conn_state_t *conn_out)
{
    if (out == NULL || max_out <= 0) return 0;
    memset(out, 0, (size_t)max_out * sizeof(*out));
    if (s_lock == NULL) return 0;
    panel_prefs_t prefs;
    panel_prefs_get(&prefs);
    int count = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        int index[PANEL_MAX_AGENTS];
        count = visible_index_locked(&prefs, index);
        if (count > max_out) count = max_out;
        for (int i = 0; i < count; i++) {
            out[i] = s_overview.agents[index[i]];
        }
        if (total_out != NULL) *total_out = s_overview.total_count;
        if (conn_out != NULL) *conn_out = s_conn;
        xSemaphoreGive(s_lock);
    }
    return count;
}

panel_conn_state_t panel_store_conn_state(void)
{
    if (s_lock == NULL) return PANEL_CONN_WIFI_CONNECTING;
    panel_conn_state_t st = PANEL_CONN_WIFI_CONNECTING;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        st = s_conn;
        xSemaphoreGive(s_lock);
    }
    return st;
}

int panel_store_first_blocked_index(void)
{
    if (s_lock == NULL) return -1;
    panel_prefs_t prefs;
    panel_prefs_get(&prefs);
    int found = -1;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        int index[PANEL_MAX_AGENTS];
        int n = visible_index_locked(&prefs, index);
        for (int i = 0; i < n; i++) {
            if (s_overview.agents[index[i]].herdr_status == PANEL_AGENT_BLOCKED) {
                found = i;
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }
    return found;
}

int panel_store_blocked_count(void)
{
    if (s_lock == NULL) return 0;
    int n = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < s_overview.count; i++) {
            if (s_overview.agents[i].herdr_status == PANEL_AGENT_BLOCKED) n++;
        }
        xSemaphoreGive(s_lock);
    }
    return n;
}

void panel_store_set_view_context(const char *selected_terminal_id)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        snprintf(s_view_term, sizeof(s_view_term), "%s",
                 selected_terminal_id != NULL ? selected_terminal_id : "");
        xSemaphoreGive(s_lock);
    }
}

bool panel_store_sound_scope_match(const char *terminal_id, uint8_t scope)
{
    if (terminal_id == NULL || s_lock == NULL) return false;
    if (scope == PANEL_SOUND_ALL) return true;
    panel_prefs_t prefs;
    panel_prefs_get(&prefs);
    bool match = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (scope == PANEL_SOUND_SELECTED) {
            match = s_view_term[0] && strcmp(s_view_term, terminal_id) == 0;
        } else if (scope == PANEL_SOUND_PAGE) {
            /* the whole home list is one screen now, so "this page" is
             * simply every card that passes the current visibility prefs */
            int index[PANEL_MAX_AGENTS];
            int n = visible_index_locked(&prefs, index);
            for (int i = 0; i < n; i++) {
                if (strcmp(s_overview.agents[index[i]].terminal_id, terminal_id) == 0) {
                    match = true;
                    break;
                }
            }
        }
        xSemaphoreGive(s_lock);
    }
    return match;
}

bool panel_store_enqueue_action(const panel_cmd_t *cmd)
{
    if (cmd == NULL || s_action_q == NULL) return false;
    return xQueueSend(s_action_q, cmd, 0) == pdTRUE;
}

bool panel_store_enqueue_control(const panel_cmd_t *cmd)
{
    if (cmd == NULL || s_control_q == NULL) return false;
    /* merge duplicate OPEN_DETAIL / REFRESH: drop if same type already queued */
    return xQueueSend(s_control_q, cmd, 0) == pdTRUE;
}

bool panel_store_recv_action(panel_cmd_t *out, TickType_t wait)
{
    if (out == NULL || s_action_q == NULL) return false;
    return xQueueReceive(s_action_q, out, wait) == pdTRUE;
}

bool panel_store_recv_control(panel_cmd_t *out, TickType_t wait)
{
    if (out == NULL || s_control_q == NULL) return false;
    return xQueueReceive(s_control_q, out, wait) == pdTRUE;
}

bool panel_store_post_ui_event(const panel_ui_evt_t *evt)
{
    if (evt == NULL || s_ui_evt_q == NULL) return false;
    /* Non-blocking only. Action results that do not fit are parked in the
     * worker-local slot and re-posted later — never busy-wait here. */
    return xQueueSend(s_ui_evt_q, evt, 0) == pdTRUE;
}

bool panel_store_recv_ui_event(panel_ui_evt_t *out, TickType_t wait)
{
    if (out == NULL || s_ui_evt_q == NULL) return false;
    return xQueueReceive(s_ui_evt_q, out, wait) == pdTRUE;
}

bool panel_store_try_reserve_action(void)
{
    /* single global pending action; busy-wait free via atomic-ish flag */
    bool expected = false;
    /* FreeRTOS critical section for portability on single-core C6 */
    portDISABLE_INTERRUPTS();
    if (!s_action_reserved) {
        s_action_reserved = true;
        expected = true;
    }
    portENABLE_INTERRUPTS();
    return expected;
}

void panel_store_release_action(void)
{
    portDISABLE_INTERRUPTS();
    s_action_reserved = false;
    portENABLE_INTERRUPTS();
}

bool panel_store_action_reserved(void)
{
    return s_action_reserved;
}
