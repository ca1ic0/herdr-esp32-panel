#pragma once

/*
 * Data model for the Herdr panel.
 *
 * Mirrors the JSON returned by the herdr-restful backend
 * (GET /api/v1/panes -> {"type":"pane_list","panes":[...]}).
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define HERDR_MAX_SESSIONS   24
#define HERDR_PANE_ID_LEN    32
#define HERDR_NAME_LEN       64
#define HERDR_CWD_LEN        128
#define HERDR_AGENT_LEN      32
#define HERDR_STATE_LEN      48

typedef enum {
    HERDR_ST_UNKNOWN = 0,
    HERDR_ST_IDLE,
    HERDR_ST_WORKING,
    HERDR_ST_BLOCKED,
    HERDR_ST_DONE,
} herdr_status_t;

typedef struct {
    char pane_id[HERDR_PANE_ID_LEN];
    char title[HERDR_NAME_LEN];     /* display_agent || title || label || pane_id */
    char agent[HERDR_AGENT_LEN];
    char cwd[HERDR_CWD_LEN];
    char state[HERDR_STATE_LEN];    /* free-form state label reported by the agent */
    herdr_status_t status;
    bool focused;
} herdr_session_t;

typedef struct {
    herdr_session_t sessions[HERDR_MAX_SESSIONS];
    int count;
    bool backend_ok;
} herdr_snapshot_t;

static inline herdr_status_t herdr_status_from_str(const char *s)
{
    if (s == NULL) return HERDR_ST_UNKNOWN;
    if (strcmp(s, "working") == 0) return HERDR_ST_WORKING;
    if (strcmp(s, "blocked") == 0) return HERDR_ST_BLOCKED;
    if (strcmp(s, "done") == 0) return HERDR_ST_DONE;
    if (strcmp(s, "idle") == 0) return HERDR_ST_IDLE;
    return HERDR_ST_UNKNOWN;
}

static inline const char *herdr_status_name(herdr_status_t st)
{
    switch (st) {
    case HERDR_ST_WORKING: return "WORKING";
    case HERDR_ST_BLOCKED: return "BLOCKED";
    case HERDR_ST_DONE:    return "DONE";
    case HERDR_ST_IDLE:    return "IDLE";
    default:               return "UNKNOWN";
    }
}

/* RGB hex colors matching the herdr dashboard palette. */
static inline unsigned herdr_status_color(herdr_status_t st)
{
    switch (st) {
    case HERDR_ST_WORKING: return 0xf2c14e;
    case HERDR_ST_BLOCKED: return 0xe5534b;
    case HERDR_ST_DONE:    return 0x57ab5a;
    case HERDR_ST_IDLE:    return 0x6b7785;
    default:               return 0x4a5563;
    }
}
