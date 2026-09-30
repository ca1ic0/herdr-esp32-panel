#pragma once

/*
 * Shared state between the network task (producer) and the LVGL UI (consumer).
 */

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "herdr_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_MSG_SEND_TEXT      0
#define PANEL_MSG_FETCH_OUTPUT   1

typedef struct {
    int type;
    char pane_id[HERDR_PANE_ID_LEN];
    char text[64];
} panel_msg_t;

extern herdr_snapshot_t g_snap;
extern SemaphoreHandle_t g_snap_lock;
extern volatile bool g_snap_dirty;

extern char g_output_buf[2048];
extern volatile bool g_output_dirty;

extern char g_toast[128];
extern volatile bool g_toast_dirty;

extern char g_conn[64];
extern volatile bool g_conn_dirty;

extern QueueHandle_t g_msg_queue;

void panel_state_init(void);

/** Remember which pane the detail page shows ("" = none). */
void panel_set_detail_pane(const char *pane_id);

/** Copy the detail pane id ("" when the detail page is closed). */
void panel_get_detail_pane(char *buf, size_t buflen);

/** Queue a "type text + Enter" request for the given pane. */
bool panel_send_text(const char *pane_id, const char *text);

/** Queue a request to re-read the pane's recent output. */
bool panel_fetch_output(const char *pane_id);

/** Set the toast line shown at the bottom of the detail page. */
void panel_set_toast(const char *text);

#ifdef __cplusplus
}
#endif
