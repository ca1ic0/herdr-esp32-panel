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
#include "wifi_connect.h"

static const char *TAG = "panel_worker";

#define OVERVIEW_PERIOD_MS      3000
#define DETAIL_PERIOD_MS        2000
#define BACKOFF_MAX_MS          15000
#define DELIVERED_CONFIRM_MS    5000
#define DIAG_PERIOD_MS          60000

static TaskHandle_t s_task;
static volatile bool s_kick_refresh;

static char s_selected_term[PANEL_TERM_ID_LEN];
static uint32_t s_selection_epoch;
static char s_server_id[PANEL_SERVER_ID_LEN];

static int64_t s_next_overview_ms;
static int64_t s_next_detail_ms;
static int s_backoff_ms;
static int64_t s_next_diag_ms;
static const char *s_http_phase = "boot";
static panel_action_state_t s_action_state = PANEL_ACTION_UNAVAILABLE;
static char s_action_rid[PANEL_REQUEST_ID_LEN];
static int64_t s_action_sent_ms;
static uint32_t s_action_epoch;

/* ---- request_id: CSPRNG 128-bit hex ---------------------------------- */

static void generate_request_id(char *out, size_t len)
{
    if (out == NULL || len < PANEL_REQUEST_ID_LEN) return;
    uint32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = esp_random();
    snprintf(out, len, "%08lx-%04x-%04x-%04x-%08lx%04x",
             (unsigned long)r[0],
             (unsigned)(r[1] >> 16),
             (unsigned)(r[1] & 0xffff),
             (unsigned)(r[2] >> 16),
             (unsigned long)(r[2] & 0xffff),
             (unsigned)(r[3] >> 16));
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
        snprintf(result.message, sizeof(result.message), "Result unknown");
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
    snprintf(r.message, sizeof(r.message), "Result unconfirmed");
    s_action_state = PANEL_ACTION_UNCERTAIN;
    panel_store_release_action();
    post_action_result(&r, s_action_epoch);
}

/* ---- polling ---------------------------------------------------------- */

static void poll_overview(int64_t now_ms)
{
    if (!wifi_is_connected()) {
        panel_store_set_conn(PANEL_CONN_WIFI_CONNECTING, "Connecting Wi-Fi");
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
        s_next_overview_ms = now_ms + OVERVIEW_PERIOD_MS;
    } else if (hr == PANEL_HTTP_AUTH) {
        panel_store_set_conn(PANEL_CONN_GATEWAY_OFFLINE, "Auth failed");
        s_next_overview_ms = now_ms + OVERVIEW_PERIOD_MS;
    } else if (hr == PANEL_HTTP_PROTO) {
        panel_store_set_conn(PANEL_CONN_GATEWAY_OFFLINE, "Protocol mismatch");
        s_next_overview_ms = now_ms + OVERVIEW_PERIOD_MS;
    } else {
        panel_store_set_conn(PANEL_CONN_GATEWAY_OFFLINE, "Server offline");
        s_backoff_ms = s_backoff_ms == 0 ? 1000 :
                       (s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2);
        s_next_overview_ms = now_ms + s_backoff_ms;
    }
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

        /* 1. confirmed actions first */
        if (panel_store_recv_action(&cmd, 0)) {
            run_action(&cmd);
            continue;
        }

        /* 2. control commands */
        if (panel_store_recv_control(&cmd, 0)) {
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
            case PANEL_CMD_SET_DISPLAY_PREF:
                /* display prefs are UI-local; ack for toast if needed */
                break;
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
        }
        if (now_ms >= s_next_overview_ms) {
            poll_overview(now_ms);
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
