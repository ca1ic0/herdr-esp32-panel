#include "panel_state.h"

#include <stdio.h>
#include <string.h>

herdr_snapshot_t g_snap;
SemaphoreHandle_t g_snap_lock;
volatile bool g_snap_dirty;

char g_output_buf[2048];
volatile bool g_output_dirty;

char g_toast[128];
volatile bool g_toast_dirty;

char g_conn[64];
volatile bool g_conn_dirty;

QueueHandle_t g_msg_queue;

static char s_detail_pane[HERDR_PANE_ID_LEN];

void panel_state_init(void)
{
    g_snap_lock = xSemaphoreCreateMutex();
    g_msg_queue = xQueueCreate(8, sizeof(panel_msg_t));
    s_detail_pane[0] = '\0';
    g_snap_dirty = false;
    g_output_dirty = false;
    g_toast_dirty = false;
    g_conn_dirty = false;
    snprintf(g_toast, sizeof(g_toast), "ready");
    snprintf(g_conn, sizeof(g_conn), "connecting...");
}

bool panel_send_text(const char *pane_id, const char *text)
{
    if (pane_id == NULL || text == NULL || g_msg_queue == NULL) {
        return false;
    }
    panel_msg_t msg = { .type = PANEL_MSG_SEND_TEXT };
    snprintf(msg.pane_id, sizeof(msg.pane_id), "%s", pane_id);
    snprintf(msg.text, sizeof(msg.text), "%s", text);
    return xQueueSend(g_msg_queue, &msg, pdMS_TO_TICKS(500)) == pdTRUE;
}

bool panel_fetch_output(const char *pane_id)
{
    if (pane_id == NULL || g_msg_queue == NULL) {
        return false;
    }
    panel_msg_t msg = { .type = PANEL_MSG_FETCH_OUTPUT };
    snprintf(msg.pane_id, sizeof(msg.pane_id), "%s", pane_id);
    return xQueueSend(g_msg_queue, &msg, pdMS_TO_TICKS(500)) == pdTRUE;
}

void panel_set_toast(const char *text)
{
    snprintf(g_toast, sizeof(g_toast), "%s", text != NULL ? text : "");
    g_toast_dirty = true;
}

void panel_set_detail_pane(const char *pane_id)
{
    snprintf(s_detail_pane, sizeof(s_detail_pane), "%s", pane_id != NULL ? pane_id : "");
}

void panel_get_detail_pane(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return;
    }
    snprintf(buf, buflen, "%s", s_detail_pane);
}
