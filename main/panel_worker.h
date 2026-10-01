#pragma once

/*
 * panel_worker: the only FreeRTOS task that talks HTTP.
 *
 * Owns: request scheduling, poll timers, backoff, action request_id
 * generation, result state machine. Publishes to panel_store.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void panel_worker_start(void);

/** Kick an immediate overview refresh (e.g. Wi-Fi just reconnected). */
void panel_worker_request_refresh(void);

#ifdef __cplusplus
}
#endif
