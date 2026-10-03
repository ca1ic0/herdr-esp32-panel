/*
 * panel_api_client — sole HTTP + JSON boundary for the Herdr panel.
 *
 * All parsing happens here, into fixed panel_model_t structures.
 * No cJSON tree is retained past the request.
 */

#include "panel_api_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "app_config.h"

static const char *TAG = "panel_api";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} resp_buf_t;

/* Single-owner worker task: the active response buffer and the persistent
 * client handle are file-scope and never touched from other tasks. */
static resp_buf_t *s_rb;
static esp_http_client_handle_t s_client;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = s_rb;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb != NULL && evt->data_len > 0) {
        size_t add = (size_t)evt->data_len;
        if (rb->len + add >= rb->cap) {
            rb->overflow = true;
            return ESP_OK;
        }
        memcpy(rb->buf + rb->len, evt->data, add);
        rb->len += add;
        rb->buf[rb->len] = '\0';
    }
    return ESP_OK;
}

static panel_http_result_t map_transport(esp_err_t err, int status, bool overflow)
{
    if (overflow) return PANEL_HTTP_TOO_LARGE;
    if (err != ESP_OK) return (status == 0) ? PANEL_HTTP_OFFLINE : PANEL_HTTP_ERR;
    if (status == 401 || status == 403) return PANEL_HTTP_AUTH;
    if (status == 404) return PANEL_HTTP_GONE;
    if (status == 409) return PANEL_HTTP_STALE; /* refined by body later */
    if (status == 422) return PANEL_HTTP_UNSUPPORTED;
    if (status == 503) return PANEL_HTTP_OFFLINE;
    if (status < 200 || status >= 300) return PANEL_HTTP_ERR;
    return PANEL_HTTP_OK;
}

/*
 * Perform one request on the persistent client handle (architecture §5.3:
 * one owner task, serialised requests, connection reused across polls).
 * `resp`/`resp_cap` must be a static/worker buffer.
 * timeout_ms: 2000 for reads, 5000 for actions.
 */
static void drop_client(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static panel_http_result_t http_request(esp_http_client_method_t method,
                                        const char *path,
                                        const char *body,
                                        char *resp, size_t resp_cap,
                                        int timeout_ms,
                                        int *status_out)
{
    char base[96];
    app_config_base_url(base, sizeof(base));

    char url[256];
    snprintf(url, sizeof(url), "%s%s", base, path);

    resp_buf_t rb = { .buf = resp, .len = 0, .cap = resp_cap, .overflow = false };
    if (resp != NULL && resp_cap > 0) resp[0] = '\0';
    s_rb = &rb;

    if (s_client == NULL) {
        esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = http_event,
            .buffer_size = 2048,
            .timeout_ms = timeout_ms,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) return PANEL_HTTP_ERR;
    } else {
        esp_http_client_set_url(s_client, url);
        esp_http_client_set_timeout_ms(s_client, timeout_ms);
    }
    esp_http_client_set_method(s_client, method);

    char auth[128];
    app_config_auth_header(auth, sizeof(auth));
    if (auth[0] != '\0') {
        esp_http_client_set_header(s_client, "Authorization", auth);
    }
    if (body != NULL) {
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_client, body, (int)strlen(body));
    } else {
        esp_http_client_set_post_field(s_client, NULL, 0);
    }

    esp_err_t err = esp_http_client_perform(s_client);
    int status = esp_http_client_get_status_code(s_client);
    if (status_out != NULL) *status_out = status;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s -> HTTP %d", esp_err_to_name(err), path, status);
        /* broken connection: rebuild the handle next request */
        drop_client();
    }
    return map_transport(err, status, rb.overflow);
}

/* ---- field helpers ---------------------------------------------------- */

static void copy_str(cJSON *obj, const char *key, char *dst, size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL && dst_len > 0) {
        snprintf(dst, dst_len, "%s", item->valuestring);
        panel_sanitize_text(dst, dst_len);
    }
}

static bool string_fits(cJSON *obj, const char *key, size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring != NULL &&
           strlen(item->valuestring) < dst_len;
}

static bool check_schema(cJSON *root)
{
    cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    return cJSON_IsNumber(sv) && sv->valueint == 1;
}

/* ---- overview --------------------------------------------------------- */

static panel_http_result_t parse_overview(const char *json, panel_overview_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return PANEL_HTTP_PROTO;
    if (!check_schema(root)) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }

    memset(out, 0, sizeof(*out));
    out->schema_version = 1;
    out->valid = true;

    if (!string_fits(root, "server_id", sizeof(out->server_id))) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }

    copy_str(root, "server_id", out->server_id, sizeof(out->server_id));

    cJSON *health = cJSON_GetObjectItemCaseSensitive(root, "health");
    if (cJSON_IsString(health) && health->valuestring != NULL &&
        strcmp(health->valuestring, "degraded") == 0) {
        out->health = PANEL_CONN_HERDR_DEGRADED;
    } else {
        out->health = PANEL_CONN_ONLINE;
    }

    cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total_count");
    out->total_count = cJSON_IsNumber(total) ? total->valueint : 0;

    cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
    if (!cJSON_IsArray(agents)) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }

    /* reject duplicate terminal_id */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, agents) {
        if (out->count >= PANEL_MAX_AGENTS) break;
        panel_agent_card_t *c = &out->agents[out->count];
        if (!string_fits(item, "terminal_id", sizeof(c->terminal_id)) ||
            !string_fits(item, "pane_id", sizeof(c->pane_id))) {
            cJSON_Delete(root);
            return PANEL_HTTP_PROTO;
        }
        copy_str(item, "terminal_id", c->terminal_id, sizeof(c->terminal_id));
        if (c->terminal_id[0] == '\0') continue;

        for (int i = 0; i < out->count; i++) {
            if (strcmp(out->agents[i].terminal_id, c->terminal_id) == 0) {
                cJSON_Delete(root);
                ESP_LOGW(TAG, "duplicate terminal_id %s", c->terminal_id);
                return PANEL_HTTP_PROTO;
            }
        }

        copy_str(item, "pane_id", c->pane_id, sizeof(c->pane_id));
        copy_str(item, "agent", c->agent, sizeof(c->agent));
        copy_str(item, "display_name", c->display_name, sizeof(c->display_name));
        copy_str(item, "workspace_label", c->workspace_label, sizeof(c->workspace_label));
        copy_str(item, "cwd_tail", c->cwd_tail, sizeof(c->cwd_tail));
        copy_str(item, "herdr_status", c->status_raw, sizeof(c->status_raw));
        c->herdr_status = panel_agent_state_from_str(c->status_raw);

        cJSON *rev = cJSON_GetObjectItemCaseSensitive(item, "revision");
        c->revision = cJSON_IsNumber(rev) ? (uint32_t)rev->valueint : 0;
        copy_str(item, "updated_at", c->updated_at, sizeof(c->updated_at));

        out->count++;
    }

    cJSON_Delete(root);
    return PANEL_HTTP_OK;
}

panel_http_result_t panel_api_fetch_overview(panel_overview_t *out)
{
    if (out == NULL) return PANEL_HTTP_ERR;

    static char resp[PANEL_API_OVERVIEW_CAP];
    int status = 0;
    panel_http_result_t r = http_request(HTTP_METHOD_GET, "/api/v1/panel/overview",
                                         NULL, resp, sizeof(resp), 2000, &status);
    if (r != PANEL_HTTP_OK) return r;

    r = parse_overview(resp, out);
    if (r == PANEL_HTTP_OK) {
        ESP_LOGI(TAG, "overview: %d/%d agents", out->count, out->total_count);
    }
    return r;
}

/* ---- detail ----------------------------------------------------------- */

static panel_http_result_t parse_detail(const char *json, uint32_t epoch,
                                        panel_detail_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return PANEL_HTTP_PROTO;
    if (!check_schema(root)) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->selection_epoch = epoch;

    cJSON *pending = cJSON_GetObjectItemCaseSensitive(root, "pending");
    if (!string_fits(root, "server_id", sizeof(out->server_id)) ||
        !string_fits(root, "terminal_id", sizeof(out->terminal_id)) ||
        !string_fits(root, "pane_id", sizeof(out->pane_id)) ||
        (cJSON_IsObject(pending) &&
         !string_fits(pending, "context_token", sizeof(out->pending.context_token)))) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }

    copy_str(root, "server_id", out->server_id, sizeof(out->server_id));
    copy_str(root, "terminal_id", out->terminal_id, sizeof(out->terminal_id));
    copy_str(root, "pane_id", out->pane_id, sizeof(out->pane_id));
    copy_str(root, "agent", out->agent, sizeof(out->agent));
    copy_str(root, "herdr_status", out->status_raw, sizeof(out->status_raw));
    out->herdr_status = panel_agent_state_from_str(out->status_raw);

    cJSON *lines = cJSON_GetObjectItemCaseSensitive(root, "output_lines");
    if (cJSON_IsArray(lines)) {
        cJSON *ln = NULL;
        cJSON_ArrayForEach(ln, lines) {
            if (out->output_line_count >= PANEL_OUTPUT_LINES) break;
            if (!cJSON_IsString(ln) || ln->valuestring == NULL) continue;
            snprintf(out->output_lines[out->output_line_count],
                     PANEL_OUTPUT_LINE_LEN, "%s", ln->valuestring);
            panel_sanitize_text(out->output_lines[out->output_line_count],
                                PANEL_OUTPUT_LINE_LEN);
            out->output_line_count++;
        }
    }

    if (cJSON_IsObject(pending)) {
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(pending, "kind");
        if (cJSON_IsString(kind) && kind->valuestring != NULL) {
            const char *k = kind->valuestring;
            if (strcmp(k, "approval") == 0) out->pending.kind = PANEL_PENDING_APPROVAL;
            else if (strcmp(k, "question") == 0) out->pending.kind = PANEL_PENDING_QUESTION;
            else if (strcmp(k, "continuation") == 0) out->pending.kind = PANEL_PENDING_CONTINUATION;
            else if (strcmp(k, "unrecognized") == 0) out->pending.kind = PANEL_PENDING_UNRECOGNIZED;
            else out->pending.kind = PANEL_PENDING_NONE;
        }
        copy_str(pending, "summary", out->pending.summary, sizeof(out->pending.summary));
        copy_str(pending, "impact", out->pending.impact, sizeof(out->pending.impact));
        copy_str(pending, "context_token", out->pending.context_token,
                 sizeof(out->pending.context_token));
        copy_str(pending, "prompt", out->pending.prompt, sizeof(out->pending.prompt));

        cJSON *choices = cJSON_GetObjectItemCaseSensitive(pending, "choices");
        if (cJSON_IsArray(choices)) {
            cJSON *ch = NULL;
            cJSON_ArrayForEach(ch, choices) {
                if (!cJSON_IsString(ch) || ch->valuestring == NULL) continue;
                if (strcmp(ch->valuestring, "allow_once") == 0)
                    out->pending.choices |= PANEL_CHOICE_ALLOW_ONCE;
                else if (strcmp(ch->valuestring, "allow_always") == 0)
                    out->pending.choices |= PANEL_CHOICE_ALLOW_ALWAYS;
                else if (strcmp(ch->valuestring, "deny") == 0)
                    out->pending.choices |= PANEL_CHOICE_DENY;
                else if (strcmp(ch->valuestring, "continue") == 0)
                    out->pending.choices |= PANEL_CHOICE_CONTINUE;
            }
        }

        /* The confirmation screen must never approve a card whose text was
         * truncated by the device's bounded model. Keep the detail readable,
         * but force the decision to be made on the host. */
        cJSON *source = cJSON_GetObjectItemCaseSensitive(pending, "source");
        bool new_prompt = cJSON_IsString(source) &&
                          strcmp(source->valuestring, "new_prompt") == 0;
        bool terminal_ui = cJSON_IsString(source) &&
                           strcmp(source->valuestring, "terminal_ui") == 0;
        out->pending.source = new_prompt ? PANEL_SOURCE_NEW_PROMPT :
                              terminal_ui ? PANEL_SOURCE_TERMINAL_UI :
                              PANEL_SOURCE_NONE;
        bool has_continue = (out->pending.choices & PANEL_CHOICE_CONTINUE) != 0;
        if (out->pending.choices != 0 &&
            (!string_fits(pending, "summary", sizeof(out->pending.summary)) ||
             !string_fits(pending, "impact", sizeof(out->pending.impact)) ||
             out->pending.summary[0] == '\0' ||
             out->pending.impact[0] == '\0' ||
             (has_continue && !new_prompt && !terminal_ui) ||
             (has_continue && new_prompt &&
              (!string_fits(pending, "prompt", sizeof(out->pending.prompt)) ||
               out->pending.prompt[0] == '\0')) ||
             (has_continue && terminal_ui &&
              cJSON_IsString(cJSON_GetObjectItemCaseSensitive(pending, "prompt")) &&
              !string_fits(pending, "prompt", sizeof(out->pending.prompt))))) {
            out->pending.kind = PANEL_PENDING_UNRECOGNIZED;
            out->pending.choices = 0;
            out->pending.context_token[0] = '\0';
            out->pending.impact[0] = '\0';
            out->pending.prompt[0] = '\0';
            snprintf(out->pending.summary, sizeof(out->pending.summary),
                     "内容过长或缺失，请在主机处理");
        } else if (terminal_ui) {
            /* A terminal choice sends keys, never a free-form prompt. */
            out->pending.prompt[0] = '\0';
        }
    }

    cJSON_Delete(root);
    return PANEL_HTTP_OK;
}

panel_http_result_t panel_api_fetch_detail(const char *terminal_id,
                                           uint32_t epoch,
                                           panel_detail_t *out)
{
    if (terminal_id == NULL || out == NULL) return PANEL_HTTP_ERR;

    char path[192];
    snprintf(path, sizeof(path), "/api/v1/panel/agents/%s", terminal_id);

    static char resp[PANEL_API_DETAIL_CAP];
    int status = 0;
    panel_http_result_t r = http_request(HTTP_METHOD_GET, path, NULL,
                                         resp, sizeof(resp), 2000, &status);
    if (r != PANEL_HTTP_OK) return r;

    r = parse_detail(resp, epoch, out);
    if (r == PANEL_HTTP_OK &&
        strcmp(out->terminal_id, terminal_id) != 0) {
        /* response identity mismatch — discard */
        memset(out, 0, sizeof(*out));
        return PANEL_HTTP_PROTO;
    }
    return r;
}

/* ---- action ----------------------------------------------------------- */

static panel_http_result_t parse_action_result(const char *json,
                                               const char *expect_rid,
                                               int status,
                                               panel_action_result_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->request_id, sizeof(out->request_id), "%s", expect_rid);

    cJSON *root = cJSON_Parse(json);
    if (root != NULL) {
        /* A partial or unrelated response must never be presented as a
         * successful action for this request. */
        cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        if (!check_schema(root) || !cJSON_IsString(rid) ||
            strcmp(rid->valuestring, expect_rid) != 0) {
            cJSON_Delete(root);
            out->state = PANEL_ACTION_UNCERTAIN;
            snprintf(out->message, sizeof(out->message), "Invalid action response");
            return PANEL_HTTP_OK;
        }
        copy_str(root, "request_id", out->request_id, sizeof(out->request_id));
        copy_str(root, "message", out->message, sizeof(out->message));

        cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        const char *res = cJSON_IsString(result) ? result->valuestring : "";
        if (strcmp(res, "accepted") == 0 || strcmp(res, "observed") == 0) {
            out->state = (strcmp(res, "observed") == 0)
                             ? PANEL_ACTION_OBSERVED : PANEL_ACTION_DELIVERED;
        } else if (strcmp(res, "uncertain") == 0) {
            out->state = PANEL_ACTION_UNCERTAIN;
        } else if (strcmp(res, "stale_context") == 0) {
            out->state = PANEL_ACTION_STALE;
        } else if (strcmp(res, "unsupported") == 0) {
            out->state = PANEL_ACTION_UNSUPPORTED;
        } else if (strcmp(res, "unauthorized") == 0) {
            out->state = PANEL_ACTION_UNAVAILABLE;
        } else if (strcmp(res, "offline") == 0) {
            out->state = PANEL_ACTION_UNCERTAIN;
        } else if (status == 409) {
            out->state = PANEL_ACTION_STALE;
        } else {
            out->state = PANEL_ACTION_UNCERTAIN;
        }
        cJSON_Delete(root);
    } else {
        /* non-JSON body */
        if (status == 401 || status == 403) {
            out->state = PANEL_ACTION_UNAVAILABLE;
            snprintf(out->message, sizeof(out->message), "Auth failed");
        } else if (status == 409) {
            out->state = PANEL_ACTION_STALE;
            snprintf(out->message, sizeof(out->message), "Context changed");
        } else if (status == 422) {
            out->state = PANEL_ACTION_UNSUPPORTED;
            snprintf(out->message, sizeof(out->message), "Unsupported");
        } else if (status == 503) {
            out->state = PANEL_ACTION_UNCERTAIN;
            snprintf(out->message, sizeof(out->message), "Server offline, result unknown");
        } else {
            out->state = PANEL_ACTION_UNCERTAIN;
            snprintf(out->message, sizeof(out->message), "Result unknown");
        }
    }

    /* Never auto-retry anything other than delivered/observed. */
    return PANEL_HTTP_OK;
}

static const char *action_to_str(panel_action_id_t a)
{
    switch (a) {
    case PANEL_ACT_ALLOW_ONCE:   return "allow_once";
    case PANEL_ACT_ALLOW_ALWAYS: return "allow_always";
    case PANEL_ACT_DENY:         return "deny";
    case PANEL_ACT_CONTINUE:     return "continue";
    /* PANEL_ACT_STOP has no gateway Action literal yet, so it is never sent;
     * the UI shows it as an unavailable hint instead of offering a request
     * the gateway would reject. */
    default:                     return NULL;
    }
}

panel_http_result_t panel_api_post_action(const panel_action_cmd_t *cmd,
                                          panel_action_result_t *result_out)
{
    if (cmd == NULL || result_out == NULL) return PANEL_HTTP_ERR;
    const char *act = action_to_str(cmd->action);
    if (act == NULL) return PANEL_HTTP_ERR;

    char path[192];
    snprintf(path, sizeof(path), "/api/v1/panel/agents/%s/actions", cmd->terminal_id);

    char prompt_esc[2 * PANEL_PROMPT_LEN];
    prompt_esc[0] = '\0';
    if (cmd->prompt[0] != '\0') {
        /* minimal escape for JSON string */
        size_t j = 0;
        for (size_t i = 0; cmd->prompt[i] != '\0' && j + 2 < sizeof(prompt_esc); i++) {
            char c = cmd->prompt[i];
            if (c == '"' || c == '\\') {
                prompt_esc[j++] = '\\';
                prompt_esc[j++] = c;
            } else if (c == '\n') {
                prompt_esc[j++] = '\\';
                prompt_esc[j++] = 'n';
            } else if ((unsigned char)c >= 0x20) {
                prompt_esc[j++] = c;
            }
        }
        prompt_esc[j] = '\0';
    }

    char body[2048];
    int body_len;
    if (cmd->prompt[0] != '\0') {
        body_len = snprintf(body, sizeof(body),
                 "{\"action\":\"%s\",\"context_token\":\"%s\",\"request_id\":\"%s\",\"prompt\":\"%s\"}",
                 act, cmd->context_token, cmd->request_id, prompt_esc);
    } else {
        body_len = snprintf(body, sizeof(body),
                 "{\"action\":\"%s\",\"context_token\":\"%s\",\"request_id\":\"%s\"}",
                 act, cmd->context_token, cmd->request_id);
    }
    if (body_len < 0 || body_len >= (int)sizeof(body)) return PANEL_HTTP_PROTO;

    static char resp[PANEL_API_ACTION_CAP];
    int status = 0;
    panel_http_result_t r = http_request(HTTP_METHOD_POST, path, body,
                                         resp, sizeof(resp), 5000, &status);
    if (r == PANEL_HTTP_AUTH) {
        memset(result_out, 0, sizeof(*result_out));
        snprintf(result_out->request_id, sizeof(result_out->request_id),
                 "%s", cmd->request_id);
        result_out->state = PANEL_ACTION_UNAVAILABLE;
        snprintf(result_out->message, sizeof(result_out->message), "Auth failed");
        return PANEL_HTTP_OK;
    }
    if (r != PANEL_HTTP_OK && r != PANEL_HTTP_STALE &&
        r != PANEL_HTTP_UNSUPPORTED && r != PANEL_HTTP_AUTH) {
        memset(result_out, 0, sizeof(*result_out));
        snprintf(result_out->request_id, sizeof(result_out->request_id),
                 "%s", cmd->request_id);
        result_out->state = PANEL_ACTION_UNCERTAIN;
        snprintf(result_out->message, sizeof(result_out->message), "Result unknown");
        return PANEL_HTTP_OK;
    }

    /* Even transport errors map to a result state (uncertain), never retry. */
    return parse_action_result(resp, cmd->request_id, status, result_out);
}

/* ---- notification events -------------------------------------------- */

static bool safe_cursor(const char *s)
{
    if (s == NULL) return false;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

panel_http_result_t panel_api_fetch_events(const char *after,
                                           panel_event_batch_t *out)
{
    if (out == NULL || after == NULL || !safe_cursor(after)) return PANEL_HTTP_PROTO;
    char path[128];
    if (after[0] == '\0') {
        snprintf(path, sizeof(path), "/api/v1/panel/events?limit=16");
    } else {
        snprintf(path, sizeof(path), "/api/v1/panel/events?after=%s&limit=16", after);
    }
    static char resp[PANEL_API_EVENTS_CAP];
    int status = 0;
    panel_http_result_t hr = http_request(HTTP_METHOD_GET, path, NULL,
                                          resp, sizeof(resp), 2000, &status);
    if (hr != PANEL_HTTP_OK) return hr;
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL || !check_schema(root)) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }
    memset(out, 0, sizeof(*out));
    if (!string_fits(root, "server_id", sizeof(out->server_id))) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }
    copy_str(root, "server_id", out->server_id, sizeof(out->server_id));
    copy_str(root, "latest_cursor", out->latest_cursor, sizeof(out->latest_cursor));
    copy_str(root, "next_cursor", out->next_cursor, sizeof(out->next_cursor));
    cJSON *gap = cJSON_GetObjectItemCaseSensitive(root, "gap");
    cJSON *more = cJSON_GetObjectItemCaseSensitive(root, "has_more");
    cJSON *events = cJSON_GetObjectItemCaseSensitive(root, "events");
    bool okay = out->server_id[0] && out->next_cursor[0] &&
                safe_cursor(out->next_cursor) && safe_cursor(out->latest_cursor) &&
                cJSON_IsBool(gap) && cJSON_IsBool(more) && cJSON_IsArray(events) &&
                cJSON_GetArraySize(events) <= PANEL_EVENT_MAX;
    if (!okay) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }
    out->gap = cJSON_IsTrue(gap);
    out->has_more = cJSON_IsTrue(more);
    cJSON *item = NULL;
    const char *previous_id = after;
    cJSON_ArrayForEach(item, events) {
        panel_notice_t *e = &out->events[out->count];
        copy_str(item, "event_id", e->event_id, sizeof(e->event_id));
        copy_str(item, "terminal_id", e->terminal_id, sizeof(e->terminal_id));
        copy_str(item, "agent", e->agent, sizeof(e->agent));
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        cJSON *age = cJSON_GetObjectItemCaseSensitive(item, "age_ms");
        if (!e->event_id[0] || !safe_cursor(e->event_id) ||
            (previous_id[0] && strcmp(e->event_id, previous_id) <= 0) ||
            !e->terminal_id[0] || !cJSON_IsString(kind) ||
            !cJSON_IsNumber(age) || age->valuedouble < 0 ||
            age->valuedouble > 3600000) {
            cJSON_Delete(root);
            return PANEL_HTTP_PROTO;
        }
        e->kind = strcmp(kind->valuestring, "request") == 0 ? 1 :
                  strcmp(kind->valuestring, "done") == 0 ? 2 : 0;
        if (e->kind == 0) {
            cJSON_Delete(root);
            return PANEL_HTTP_PROTO;
        }
        e->age_ms = (uint32_t)age->valuedouble;
        previous_id = e->event_id;
        out->count++;
    }
    if ((after[0] == '\0' && out->count != 0) ||
        (out->gap && (out->count != 0 || out->has_more ||
                      strcmp(out->next_cursor, out->latest_cursor) != 0)) ||
        (out->has_more && out->count == 0) ||
        (out->count > 0 &&
         strcmp(out->next_cursor, out->events[out->count - 1].event_id) != 0)) {
        cJSON_Delete(root);
        return PANEL_HTTP_PROTO;
    }
    cJSON_Delete(root);
    return PANEL_HTTP_OK;
}
