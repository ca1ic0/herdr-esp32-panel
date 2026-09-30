/*
 * HTTP client for the herdr-restful backend.
 *
 * The panel is a pure frontend: every request is a plain JSON REST call
 * against the FastAPI service running on the PC.
 */

#include "herdr_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "app_config.h"

static const char *TAG = "herdr_client";

#define RESP_BUF_CAP   (24 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb != NULL && evt->data_len > 0) {
        if (rb->len + (size_t)evt->data_len < rb->cap) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += (size_t)evt->data_len;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t http_request(esp_http_client_method_t method, const char *path,
                              const char *body, char *resp, size_t resp_cap,
                              int *status_out)
{
    char base[96];
    app_config_base_url(base, sizeof(base));

    char url[256];
    snprintf(url, sizeof(url), "%s%s", base, path);

    resp_buf_t rb = { .buf = resp, .len = 0, .cap = resp_cap };
    if (resp != NULL && resp_cap > 0) {
        resp[0] = '\0';
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = http_event,
        .user_data = &rb,
        .timeout_ms = 8000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (status_out != NULL) {
        *status_out = esp_http_client_get_status_code(client);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request %s failed: %s", path, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

/* Copy a JSON string field, truncating safely. */
static void copy_str(cJSON *obj, const char *key, char *dst, size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, dst_len, "%s", item->valuestring);
    }
}

esp_err_t herdr_fetch_sessions(herdr_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *resp = (char *)malloc(RESP_BUF_CAP);
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = http_request(HTTP_METHOD_GET, "/api/v1/panes",
                                 NULL, resp, RESP_BUF_CAP, &status);
    if (err != ESP_OK) {
        free(resp);
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "GET /api/v1/panes -> HTTP %d", status);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        ESP_LOGW(TAG, "response is not JSON");
        return ESP_FAIL;
    }

    cJSON *panes = cJSON_GetObjectItemCaseSensitive(root, "panes");
    if (!cJSON_IsArray(panes)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    out->backend_ok = true;

    cJSON *pane = NULL;
    cJSON_ArrayForEach(pane, panes) {
        if (out->count >= HERDR_MAX_SESSIONS) {
            break;
        }
        herdr_session_t *s = &out->sessions[out->count];
        copy_str(pane, "pane_id", s->pane_id, sizeof(s->pane_id));
        if (s->pane_id[0] == '\0') {
            continue;
        }

        /* Name precedence mirrors the reference dashboard: display_agent,
         * then title/label/terminal title, falling back to the pane id. */
        cJSON *name = cJSON_GetObjectItemCaseSensitive(pane, "display_agent");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItemCaseSensitive(pane, "agent");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItemCaseSensitive(pane, "title");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItemCaseSensitive(pane, "label");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItemCaseSensitive(pane, "terminal_title_stripped");
        if (cJSON_IsString(name) && name->valuestring != NULL) {
            snprintf(s->title, sizeof(s->title), "%s", name->valuestring);
        } else {
            /* memmove: dst/src belong to the same snapshot object */
            memmove(s->title, s->pane_id, strlen(s->pane_id) + 1);
        }

        copy_str(pane, "agent", s->agent, sizeof(s->agent));

        cJSON *cwd = cJSON_GetObjectItemCaseSensitive(pane, "foreground_cwd");
        if (!cJSON_IsString(cwd)) cwd = cJSON_GetObjectItemCaseSensitive(pane, "cwd");
        if (cJSON_IsString(cwd) && cwd->valuestring != NULL) {
            snprintf(s->cwd, sizeof(s->cwd), "%s", cwd->valuestring);
        }

        cJSON *st = cJSON_GetObjectItemCaseSensitive(pane, "agent_status");
        s->status = herdr_status_from_str(cJSON_IsString(st) ? st->valuestring : NULL);

        cJSON *focused = cJSON_GetObjectItemCaseSensitive(pane, "focused");
        s->focused = cJSON_IsTrue(focused);

        /* state_labels is a free-form map; show the first entry. */
        cJSON *labels = cJSON_GetObjectItemCaseSensitive(pane, "state_labels");
        if (cJSON_IsObject(labels) && labels->child != NULL) {
            cJSON *first = labels->child;
            snprintf(s->state, sizeof(s->state), "%s=%s",
                     first->string != NULL ? first->string : "",
                     cJSON_IsString(first) && first->valuestring != NULL ? first->valuestring : "");
        }

        out->count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "fetched %d session(s)", out->count);
    return ESP_OK;
}

/* Minimal JSON string escaping (the payload is short config text). */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < out_len; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if ((unsigned char)c < 0x20) {
            /* drop other control characters */
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

esp_err_t herdr_send_text(const char *pane_id, const char *text, bool submit)
{
    if (pane_id == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char esc[192];
    json_escape(text, esc, sizeof(esc));

    char path[128];
    snprintf(path, sizeof(path), "/api/v1/panes/%s/input/text", pane_id);

    char body[256];
    snprintf(body, sizeof(body), "{\"text\":\"%s\",\"submit\":%s}",
             esc, submit ? "true" : "false");

    char resp[512];
    int status = 0;
    esp_err_t err = http_request(HTTP_METHOD_POST, path, body, resp, sizeof(resp), &status);
    if (err != ESP_OK) {
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "send_text -> HTTP %d", status);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sent text to %s", pane_id);
    return ESP_OK;
}

esp_err_t herdr_fetch_output(const char *pane_id, char *buf, size_t buflen, int lines)
{
    if (pane_id == NULL || buf == NULL || buflen == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    char path[160];
    snprintf(path, sizeof(path), "/api/v1/panes/%s/output?source=recent&lines=%d",
             pane_id, lines > 0 ? lines : 12);

    char *resp = (char *)malloc(RESP_BUF_CAP);
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = http_request(HTTP_METHOD_GET, path, NULL, resp, RESP_BUF_CAP, &status);
    if (err != ESP_OK || status < 200 || status >= 300) {
        free(resp);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        return ESP_FAIL;
    }

    /* pane_read returns either {"lines":[...]} or {"text":"..."} */
    cJSON *lines_arr = cJSON_GetObjectItemCaseSensitive(root, "lines");
    if (cJSON_IsArray(lines_arr)) {
        size_t used = 0;
        cJSON *line = NULL;
        cJSON_ArrayForEach(line, lines_arr) {
            if (!cJSON_IsString(line) || line->valuestring == NULL) {
                continue;
            }
            int n = snprintf(buf + used, buflen - used, "%s\n", line->valuestring);
            if (n < 0 || (size_t)n >= buflen - used) {
                break;
            }
            used += (size_t)n;
        }
    } else {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            snprintf(buf, buflen, "%s", text->valuestring);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}
