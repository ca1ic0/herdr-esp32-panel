/*
 * panel_worker — single HTTP executor + action lifecycle.
 *
 * Scheduling priority (ARCHITECTURE.md §5.1):
 *   1. confirmed ACTION
 *   2. control (OPEN_DETAIL / REFRESH / RECONNECT / prefs)
 *   3. due polling (overview 3s, detail 2s)
 */

#include "panel_worker.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "panel_api_client.h"
#include "panel_model.h"
#include "panel_store.h"
#include "panel_prefs.h"
#include "panel_audio.h"
#include "wifi_connect.h"

static const char *TAG = "panel_worker";

#define DETAIL_PERIOD_MS        2000
#define BACKOFF_MAX_MS          15000
#define DELIVERED_CONFIRM_MS    5000
#define DIAG_PERIOD_MS          60000
#define EVENTS_PERIOD_MS        2000

static TaskHandle_t s_task;
static volatile bool s_kick_refresh;

static char s_selected_term[PANEL_TERM_ID_LEN];
static uint32_t s_selection_epoch;
static char s_server_id[PANEL_SERVER_ID_LEN];

static int64_t s_next_overview_ms;
static int64_t s_next_detail_ms;
static int64_t s_next_events_ms;
static int s_backoff_ms;
static int64_t s_next_diag_ms;
static const char *s_http_phase = "boot";
static panel_action_state_t s_action_state = PANEL_ACTION_UNAVAILABLE;
static char s_action_rid[PANEL_REQUEST_ID_LEN];
static int64_t s_action_sent_ms;
static uint32_t s_action_epoch;
static char s_event_cursor[PANEL_EVENT_ID_LEN];
static int64_t s_last_sound_ms;
static struct {
    char terminal_id[PANEL_TERM_ID_LEN];
    uint8_t kind;
    int64_t at_ms;
} s_recent_sound[16];
static unsigned s_recent_next;

/* ---- request_id: CSPRNG 128-bit hex ---------------------------------- */

static void generate_request_id(char *out, size_t len)
{
    if (out == NULL || len < PANEL_REQUEST_ID_LEN) return;
    uint32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = esp_random();
    snprintf(out, len, "%08lx-%04x-%04x-%04x-%04x%08lx",
             (unsigned long)r[0],
             (unsigned)(r[1] >> 16),
             (unsigned)(r[1] & 0xffff),
             (unsigned)(r[2] >> 16),
             (unsigned)(r[2] & 0xffff),
             (unsigned long)r[3]);
}

/* ---- server_id change wipes cache ------------------------------------ */

static void handle_server_id(const char *sid)
{
    if (sid == NULL || sid[0] == '\0') return;
    if (s_server_id[0] != '\0' && strcmp(s_server_id, sid) != 0) {
        ESP_LOGW(TAG, "server_id changed %s -> %s, clearing cache", s_server_id, sid);
        panel_overview_t empty = { 0 };
        panel_detail_t dempty = { 0 };
        panel_store_publish_overview(&empty);
        panel_store_publish_detail(&dempty);
        s_selected_term[0] = '\0';
        s_action_state = PANEL_ACTION_UNAVAILABLE;
        panel_store_release_action();
        s_backoff_ms = 0;
        s_event_cursor[0] = '\0';
        s_next_events_ms = 0;
    }
    snprintf(s_server_id, sizeof(s_server_id), "%s", sid);
}

/* ---- posting UI events ----------------------------------------------- */

/*
 * Action results must never be silently dropped (architecture §5.1).
 * If the UI event queue is full the event is parked in this worker-local
 * slot and re-posted on every loop iteration until it lands.
 */
static panel_ui_evt_t s_parked_evt;
static bool s_parked_evt_valid;
static panel_ui_evt_t s_parked_config_evt;
static bool s_parked_config_evt_valid;

static void post_action_result(const panel_action_result_t *r, uint32_t epoch)
{
    panel_ui_evt_t evt = { .type = PANEL_EVT_ACTION_RESULT };
    evt.action = *r;
    evt.action.selection_epoch = epoch;
    if (!panel_store_post_ui_event(&evt)) {
        s_parked_evt = evt;
        s_parked_evt_valid = true;
        ESP_LOGW(TAG, "ui_evt_q full, action result parked in worker slot");
    }
}

static void flush_parked_event(void)
{
    if (s_parked_evt_valid && panel_store_post_ui_event(&s_parked_evt)) {
        s_parked_evt_valid = false;
    }
    if (s_parked_config_evt_valid && panel_store_post_ui_event(&s_parked_config_evt)) {
        s_parked_config_evt_valid = false;
    }
}

/* ---- action path ------------------------------------------------------ */

static void run_action(panel_cmd_t *cmd)
{
    if (cmd->action == PANEL_ACT_NONE || cmd->terminal_id[0] == '\0') {
        panel_store_release_action();
        return;
    }

    char rid[PANEL_REQUEST_ID_LEN];
    generate_request_id(rid, sizeof(rid));
    snprintf(s_action_rid, sizeof(s_action_rid), "%s", rid);
    s_action_epoch = cmd->selection_epoch;
    s_action_state = PANEL_ACTION_SENDING;
    s_action_sent_ms = esp_timer_get_time() / 1000;

    panel_action_cmd_t ac = {
        .action = cmd->action,
        .selection_epoch = cmd->selection_epoch,
    };
    snprintf(ac.terminal_id, sizeof(ac.terminal_id), "%s", cmd->terminal_id);
    snprintf(ac.context_token, sizeof(ac.context_token), "%s", cmd->context_token);
    snprintf(ac.prompt, sizeof(ac.prompt), "%s", cmd->prompt);
    snprintf(ac.request_id, sizeof(ac.request_id), "%s", rid);

    panel_action_result_t result;
    s_http_phase = "action";
    panel_http_result_t hr = panel_api_post_action(&ac, &result);
    if (hr != PANEL_HTTP_OK) {
        result.state = PANEL_ACTION_UNCERTAIN;
        snprintf(result.request_id, sizeof(result.request_id), "%s", ac.request_id);
        snprintf(result.message, sizeof(result.message), "结果未知");
    }

    s_action_state = result.state;
    /* reserved slot is released only when terminal or uncertain */
    if (result.state == PANEL_ACTION_OBSERVED ||
        result.state == PANEL_ACTION_UNCERTAIN ||
        result.state == PANEL_ACTION_STALE ||
        result.state == PANEL_ACTION_UNSUPPORTED ||
        result.state == PANEL_ACTION_UNAVAILABLE) {
        panel_store_release_action();
    }
    /* DELIVERED keeps the slot until observe timeout */

    post_action_result(&result, cmd->selection_epoch);

    /* always re-read detail after an action */
    s_next_detail_ms = 0;
    s_next_overview_ms = 0;
    s_kick_refresh = true;
}

static void check_delivered_timeout(int64_t now_ms)
{
    if (s_action_state != PANEL_ACTION_DELIVERED) return;
    if (now_ms - s_action_sent_ms < DELIVERED_CONFIRM_MS) return;

    panel_action_result_t r = { 0 };
    snprintf(r.request_id, sizeof(r.request_id), "%s", s_action_rid);
    r.state = PANEL_ACTION_UNCERTAIN;
    snprintf(r.message, sizeof(r.message), "结果未确认");
    s_action_state = PANEL_ACTION_UNCERTAIN;
    panel_store_release_action();
    post_action_result(&r, s_action_epoch);
}

/* ---- polling ---------------------------------------------------------- */

static void poll_overview(int64_t now_ms)
{
    if (!wifi_is_connected()) {
        s_event_cursor[0] = '\0';
        panel_store_set_sound_notice("离线期间提醒暂停");
        panel_store_set_conn(PANEL_CONN_WIFI_CONNECTING, "连接 Wi-Fi");
        s_next_overview_ms = now_ms + 1000;
        return;
    }

    panel_overview_t ov;
    s_http_phase = "overview";
    panel_http_result_t hr = panel_api_fetch_overview(&ov);
    if (hr == PANEL_HTTP_OK) {
        handle_server_id(ov.server_id);
        ov.fetched_at_ms = now_ms;
        panel_store_publish_overview(&ov);
        panel_store_set_conn(ov.health, NULL);
        s_backoff_ms = 0;
        panel_prefs_t prefs;
        panel_prefs_get(&prefs);
        s_next_overview_ms = now_ms + prefs.overview_interval * 1000;
    } else if (hr == PANEL_HTTP_AUTH) {
        s_event_cursor[0] = '\0';
        panel_store_set_conn(PANEL_CONN_GATEWAY_OFFLINE, "鉴权失败");
        s_next_overview_ms = now_ms + 3000;
    } else if (hr == PANEL_HTTP_PROTO) {
        s_event_cursor[0] = '\0';
        panel_store_set_conn(PANEL_CONN_GATEWAY_OFFLINE, "协议不匹配");
        s_next_overview_ms = now_ms + 3000;
    } else {
        s_event_cursor[0] = '\0';
        panel_store_set_conn(PANEL_CONN_GATEWAY_OFFLINE, "服务器离线");
        s_backoff_ms = s_backoff_ms == 0 ? 1000 :
                       (s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2);
        s_next_overview_ms = now_ms + s_backoff_ms;
    }
}

static bool quiet_now(const panel_prefs_t *p)
{
    if (!p->quiet_enabled) return false;
    time_t now = time(NULL);
    if (now < 1704067200) return true; /* clock has not synced */
    now += p->utc_offset_minutes * 60;
    struct tm local;
    if (gmtime_r(&now, &local) == NULL) return true;
    int minute = local.tm_hour * 60 + local.tm_min;
    if (p->quiet_start == p->quiet_end) return true;
    if (p->quiet_start < p->quiet_end) {
        return minute >= p->quiet_start && minute < p->quiet_end;
    }
    return minute >= p->quiet_start || minute < p->quiet_end;
}

static bool sound_allowed(const panel_notice_t *e, const panel_prefs_t *p,
                          int64_t now_ms)
{
    if (!p->sound_enabled || p->sound_volume == 0 || quiet_now(p)) return false;
    if (e->age_ms > 30000 || !panel_audio_ready()) return false;
    if ((e->kind == PANEL_AUDIO_REQUEST && !p->sound_request) ||
        (e->kind == PANEL_AUDIO_DONE && !p->sound_done)) return false;
    if (!panel_store_sound_scope_match(e->terminal_id, p->sound_scope)) return false;
    int override = PANEL_SOUND_INHERIT;
    if (strcmp(e->agent, "claude") == 0) override = p->sound_claude;
    else if (strcmp(e->agent, "opencode") == 0) override = p->sound_opencode;
    else if (strcmp(e->agent, "pi") == 0) override = p->sound_pi;
    if (override == PANEL_SOUND_OFF) return false;
    if (now_ms - s_last_sound_ms < 500) return false;
    for (unsigned i = 0; i < 16; i++) {
        if (s_recent_sound[i].kind == e->kind &&
            strcmp(s_recent_sound[i].terminal_id, e->terminal_id) == 0 &&
            now_ms - s_recent_sound[i].at_ms < 15000) return false;
    }
    return true;
}

static void poll_events(int64_t now_ms)
{
    if (!wifi_is_connected() || s_server_id[0] == '\0' ||
        panel_store_conn_state() != PANEL_CONN_ONLINE) {
        s_next_events_ms = now_ms + EVENTS_PERIOD_MS;
        return;
    }
    panel_event_batch_t batch;
    bool baseline = s_event_cursor[0] == '\0';
    s_http_phase = "events";
    panel_http_result_t hr = panel_api_fetch_events(s_event_cursor, &batch);
    if (hr != PANEL_HTTP_OK) {
        if (hr == PANEL_HTTP_GONE || hr == PANEL_HTTP_PROTO) {
            ESP_LOGW(TAG, "sound event API unavailable (%d)", (int)hr);
            panel_store_set_sound_notice("声音提醒不可用");
            s_next_events_ms = now_ms + 30000;
        } else {
            if (hr == PANEL_HTTP_AUTH) panel_store_set_sound_notice("提醒接口鉴权失败");
            s_next_events_ms = now_ms + EVENTS_PERIOD_MS;
        }
        return;
    }
    if (strcmp(batch.server_id, s_server_id) != 0) {
        s_event_cursor[0] = '\0';
        s_next_overview_ms = 0;
        s_next_events_ms = now_ms + EVENTS_PERIOD_MS;
        return;
    }
    if (batch.gap) {
        panel_store_set_sound_notice("可能遗漏部分提醒");
    } else if (baseline || batch.count > 0) {
        panel_store_set_sound_notice("");
    }
    if (!baseline && !batch.gap) {
        panel_prefs_t p;
        panel_prefs_get(&p);
        for (int i = 0; i < batch.count; i++) {
            const panel_notice_t *e = &batch.events[i];
            if (!sound_allowed(e, &p, now_ms)) continue;
            if (panel_audio_play((panel_audio_kind_t)e->kind)) {
                unsigned slot = s_recent_next++ % 16;
                snprintf(s_recent_sound[slot].terminal_id,
                         sizeof(s_recent_sound[slot].terminal_id), "%s", e->terminal_id);
                s_recent_sound[slot].kind = e->kind;
                s_recent_sound[slot].at_ms = now_ms;
                s_last_sound_ms = now_ms;
            }
        }
    }
    snprintf(s_event_cursor, sizeof(s_event_cursor), "%s", batch.next_cursor);
    s_next_events_ms = now_ms + (batch.has_more ? 100 : EVENTS_PERIOD_MS);
}

static void poll_detail(int64_t now_ms)
{
    if (s_selected_term[0] == '\0') {
        s_next_detail_ms = now_ms + DETAIL_PERIOD_MS;
        return;
    }

    panel_detail_t det;
    s_http_phase = "detail";
    panel_http_result_t hr = panel_api_fetch_detail(s_selected_term,
                                                    s_selection_epoch, &det);
    if (hr == PANEL_HTTP_OK) {
        /* drop if selection moved on */
        if (det.selection_epoch != s_selection_epoch ||
            strcmp(det.terminal_id, s_selected_term) != 0) {
            ESP_LOGW(TAG, "detail response discarded (epoch/identity)");
            return;
        }
        handle_server_id(det.server_id);
        det.fetched_at_ms = now_ms;
        panel_store_publish_detail(&det);
        s_next_detail_ms = now_ms + DETAIL_PERIOD_MS;

        /* blocked without poll wait: already handled by schedule */
    } else if (hr == PANEL_HTTP_GONE) {
        /* session ended: publish a marker and stop polling it */
        panel_detail_t ended = { 0 };
        ended.valid = true;
        ended.gone = true;
        ended.fetched_at_ms = now_ms;
        snprintf(ended.terminal_id, sizeof(ended.terminal_id), "%s", s_selected_term);
        panel_store_publish_detail(&ended);
        s_next_detail_ms = INT64_MAX;
    } else {
        s_next_detail_ms = now_ms + DETAIL_PERIOD_MS;
    }
}

static void open_detail(const panel_cmd_t *cmd)
{
    snprintf(s_selected_term, sizeof(s_selected_term), "%s", cmd->terminal_id);
    s_selection_epoch = cmd->selection_epoch;
    s_next_detail_ms = 0;   /* fetch immediately */
    /* invalidate old detail */
    panel_detail_t empty = { 0 };
    panel_store_publish_detail(&empty);
}

static void close_detail(void)
{
    s_selected_term[0] = '\0';
    s_next_detail_ms = INT64_MAX;   /* stop polling an unseen detail */
    panel_detail_t empty = { 0 };
    panel_store_publish_detail(&empty);
}

/* ---- periodic diagnostics (architecture §6) --------------------------- */

static void log_diagnostics(int64_t now_ms)
{
    if (now_ms < s_next_diag_ms) return;
    s_next_diag_ms = now_ms + DIAG_PERIOD_MS;
    ESP_LOGI(TAG,
             "diag: heap_free=%u heap_min=%u heap_largest=%u stack_hwm=%u phase=%s",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL), s_http_phase);
}

/* ---- main loop -------------------------------------------------------- */

static void worker_task(void *arg)
{
    (void)arg;
    panel_cmd_t cmd;
    int64_t now_ms;

    for (;;) {
        now_ms = esp_timer_get_time() / 1000;
        flush_parked_event();

        /* 1. confirmed actions first */
        if (!s_parked_evt_valid && panel_store_recv_action(&cmd, 0)) {
            run_action(&cmd);
            continue;
        }

        /* 2. control commands */
        if (!s_parked_config_evt_valid && panel_store_recv_control(&cmd, 0)) {
            switch (cmd.type) {
            case PANEL_CMD_OPEN_DETAIL:
                open_detail(&cmd);
                break;
            case PANEL_CMD_CLOSE_DETAIL:
                close_detail();
                break;
            case PANEL_CMD_REFRESH:
            case PANEL_CMD_RECONNECT:
                s_kick_refresh = true;
                s_backoff_ms = 0;
                s_next_overview_ms = 0;
                s_next_detail_ms = 0;
                break;
            case PANEL_CMD_SET_PREF: {
                esp_err_t err = panel_prefs_set(cmd.pref_field, cmd.pref_value);
                panel_ui_evt_t evt = {
                    .type = PANEL_EVT_CONFIG_RESULT,
                    .pref_field = cmd.pref_field,
                    .edit_id = cmd.edit_id,
                    .config_saved = err == ESP_OK,
                };
                snprintf(evt.message, sizeof(evt.message), "%s",
                         err == ESP_OK ? "已保存" : "保存失败，已恢复原值");
                if (!panel_store_post_ui_event(&evt)) {
                    s_parked_config_evt = evt;
                    s_parked_config_evt_valid = true;
                }
                if (err == ESP_OK && cmd.pref_field == PANEL_PREF_OVERVIEW_INTERVAL) {
                    s_next_overview_ms = 0;
                }
                if (err == ESP_OK && cmd.pref_field == PANEL_PREF_SOUND_ENABLED &&
                    cmd.pref_value == 0) panel_audio_stop();
                if (err == ESP_OK && cmd.pref_field == PANEL_PREF_SOUND_VOLUME) {
                    panel_audio_set_volume((uint8_t)cmd.pref_value);
                }
                break;
            }
            default:
                break;
            }
            continue;
        }

        if (s_kick_refresh) {
            s_kick_refresh = false;
            s_next_overview_ms = 0;
            s_next_detail_ms = 0;
        }

        /* 3. due polls */
        if (now_ms >= s_next_detail_ms) {
            poll_detail(now_ms);
        } else if (now_ms >= s_next_overview_ms) {
            poll_overview(now_ms);
        } else if (now_ms >= s_next_events_ms) {
            poll_events(now_ms);
        }

        check_delivered_timeout(now_ms);

        flush_parked_event();
        log_diagnostics(now_ms);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void panel_worker_start(void)
{
    /* 16 KB stack: JSON parse + 6-12 KB response buffers are static,
     * but cJSON tree still needs headroom. Measured later. */
    xTaskCreatePinnedToCore(worker_task, "panel_worker", 16 * 1024, NULL, 5, &s_task, 0);
    ESP_LOGI(TAG, "worker started");
}

void panel_worker_request_refresh(void)
{
    s_kick_refresh = true;
}
