#pragma once

/*
 * Thin HTTP client for the herdr-restful backend running on the PC.
 *
 * Endpoints used (see refer/herdr-restful/README.md):
 *   GET  /api/v1/panes                    -> {"type":"pane_list","panes":[...]}
 *   GET  /api/v1/panes/{id}/output        -> {"type":"pane_read","lines":[...]}
 *   POST /api/v1/panes/{id}/input/text    -> {"text":"...","submit":true}
 */

#include "esp_err.h"
#include "herdr_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fetch the list of herdr panes (sessions) into `out`. */
esp_err_t herdr_fetch_sessions(herdr_snapshot_t *out);

/** Send text (optionally followed by Enter) to a pane. */
esp_err_t herdr_send_text(const char *pane_id, const char *text, bool submit);

/** Read the last `lines` lines of a pane's recent output. */
esp_err_t herdr_fetch_output(const char *pane_id, char *buf, size_t buflen, int lines);

#ifdef __cplusplus
}
#endif
