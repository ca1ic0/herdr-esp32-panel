#pragma once

/*
 * panel_api_client: the only HTTP client in the firmware.
 *
 * Speaks the device-facing panel gateway contract (PRODUCT_LOGIC.md §4.2):
 *   GET  /api/v1/panel/overview
 *   GET  /api/v1/panel/agents/{terminal_id}
 *   POST /api/v1/panel/agents/{terminal_id}/actions
 *
 * Bounded response buffers. Explicit size limits. Auth via Bearer token.
 */

#include "esp_err.h"
#include "panel_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Response budget from ARCHITECTURE.md §6 */
#define PANEL_API_OVERVIEW_CAP   (12 * 1024)
#define PANEL_API_DETAIL_CAP     (6 * 1024)
#define PANEL_API_ACTION_CAP     (1 * 1024)
#define PANEL_API_EVENTS_CAP     (4 * 1024)

typedef enum {
    PANEL_HTTP_OK = 0,
    PANEL_HTTP_AUTH,            /* 401/403 */
    PANEL_HTTP_STALE,           /* 409 stale_context */
    PANEL_HTTP_ID_CONFLICT,     /* 409 id_conflict */
    PANEL_HTTP_UNSUPPORTED,     /* 422 */
    PANEL_HTTP_OFFLINE,         /* 503 */
    PANEL_HTTP_PROTO,           /* schema_version / malformed */
    PANEL_HTTP_GONE,            /* 404 — session ended */
    PANEL_HTTP_TOO_LARGE,
    PANEL_HTTP_TIMEOUT,
    PANEL_HTTP_ERR,             /* other network / 5xx */
} panel_http_result_t;

/** GET overview. On success fills `out` (schema + bounds checked). */
panel_http_result_t panel_api_fetch_overview(panel_overview_t *out);

/** GET agent detail. `epoch` is stamped into `out->selection_epoch`. */
panel_http_result_t panel_api_fetch_detail(const char *terminal_id,
                                           uint32_t epoch,
                                           panel_detail_t *out);

/**
 * POST one semantic action. `cmd->request_id` must already be filled.
 * Fills `result_out` with a mapped action state + safe message.
 */
panel_http_result_t panel_api_post_action(const panel_action_cmd_t *cmd,
                                          panel_action_result_t *result_out);

/** Empty `after` establishes the current event cursor without replay. */
panel_http_result_t panel_api_fetch_events(const char *after,
                                           panel_event_batch_t *out);

#ifdef __cplusplus
}
#endif
