#pragma once

/*
 * Device-side panel data model for the /api/v1/panel gateway.
 *
 * Bounded structures only. Network parsing fills these; the UI copies
 * read-only views out of panel_store. No LVGL objects live here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits (architecture budget) ------------------------------------ */

#define PANEL_MAX_AGENTS        24
#define PANEL_TERM_ID_LEN       32
#define PANEL_PANE_ID_LEN       32
#define PANEL_AGENT_LEN         24
#define PANEL_NAME_LEN          48
#define PANEL_LABEL_LEN         32
#define PANEL_STATUS_STR_LEN    24
#define PANEL_SERVER_ID_LEN     48
#define PANEL_TOKEN_LEN         96
#define PANEL_SUMMARY_LEN       160
#define PANEL_IMPACT_LEN        160
#define PANEL_PROMPT_LEN        192
#define PANEL_OUTPUT_LINES      12
#define PANEL_OUTPUT_LINE_LEN   96
#define PANEL_REQUEST_ID_LEN    40   /* 128-bit formatted id + NUL */
#define PANEL_MESSAGE_LEN       96

/* ---- Herdr agent status (raw values, never rewritten) ---------------- */

typedef enum {
    PANEL_AGENT_UNKNOWN = 0,
    PANEL_AGENT_IDLE,
    PANEL_AGENT_WORKING,
    PANEL_AGENT_BLOCKED,
    PANEL_AGENT_DONE,
} panel_agent_state_t;

/* ---- connectivity (independent of agent status) ---------------------- */

typedef enum {
    PANEL_CONN_PROVISIONING = 0,
    PANEL_CONN_WIFI_CONNECTING,
    PANEL_CONN_GATEWAY_OFFLINE,
    PANEL_CONN_HERDR_DEGRADED,
    PANEL_CONN_ONLINE,
} panel_conn_state_t;

/* ---- action lifecycle ------------------------------------------------ */

typedef enum {
    PANEL_ACTION_UNAVAILABLE = 0,
    PANEL_ACTION_READY,
    PANEL_ACTION_CONFIRMING,
    PANEL_ACTION_SENDING,
    PANEL_ACTION_DELIVERED,
    PANEL_ACTION_OBSERVED,
    PANEL_ACTION_UNCERTAIN,
    PANEL_ACTION_STALE,
    PANEL_ACTION_UNSUPPORTED,
} panel_action_state_t;

/* ---- pending card kind ----------------------------------------------- */

typedef enum {
    PANEL_PENDING_NONE = 0,
    PANEL_PENDING_APPROVAL,
    PANEL_PENDING_QUESTION,
    PANEL_PENDING_CONTINUATION,
    PANEL_PENDING_UNRECOGNIZED,
} panel_pending_kind_t;

/* Semantic actions. Firmware never hardcodes CLI keystrokes. */
typedef enum {
    PANEL_ACT_NONE = 0,
    PANEL_ACT_ALLOW_ONCE,
    PANEL_ACT_ALLOW_ALWAYS,
    PANEL_ACT_DENY,
    PANEL_ACT_CONTINUE,
} panel_action_id_t;

/* Bitmask of available choices from the gateway. */
#define PANEL_CHOICE_ALLOW_ONCE     (1u << 0)
#define PANEL_CHOICE_ALLOW_ALWAYS   (1u << 1)
#define PANEL_CHOICE_DENY           (1u << 2)
#define PANEL_CHOICE_CONTINUE       (1u << 3)

/* ---- overview record -------------------------------------------------- */

typedef struct {
    char terminal_id[PANEL_TERM_ID_LEN];
    char pane_id[PANEL_PANE_ID_LEN];
    char agent[PANEL_AGENT_LEN];
    char display_name[PANEL_NAME_LEN];
    char workspace_label[PANEL_LABEL_LEN];
    char cwd_tail[PANEL_LABEL_LEN];
    panel_agent_state_t herdr_status;
    char status_raw[PANEL_STATUS_STR_LEN];
    uint32_t revision;
} panel_agent_card_t;

typedef struct {
    char server_id[PANEL_SERVER_ID_LEN];
    uint32_t schema_version;
    panel_conn_state_t health;      /* ok|degraded mapped to ONLINE|HERDR_DEGRADED */
    int total_count;
    int count;                      /* cards actually stored (<= PANEL_MAX_AGENTS) */
    panel_agent_card_t agents[PANEL_MAX_AGENTS];
    int64_t fetched_at_ms;          /* monotonic ms when snapshot was accepted */
    bool valid;
} panel_overview_t;

/* ---- detail + pending card -------------------------------------------- */

typedef struct {
    panel_pending_kind_t kind;
    char summary[PANEL_SUMMARY_LEN];
    char impact[PANEL_IMPACT_LEN];
    uint8_t choices;                /* PANEL_CHOICE_* bitmask */
    char context_token[PANEL_TOKEN_LEN];
    char prompt[PANEL_PROMPT_LEN];  /* full text for CONTINUE */
} panel_pending_t;

typedef struct {
    char server_id[PANEL_SERVER_ID_LEN];
    char terminal_id[PANEL_TERM_ID_LEN];
    char pane_id[PANEL_PANE_ID_LEN];
    char agent[PANEL_AGENT_LEN];
    panel_agent_state_t herdr_status;
    char status_raw[PANEL_STATUS_STR_LEN];
    int output_line_count;
    char output_lines[PANEL_OUTPUT_LINES][PANEL_OUTPUT_LINE_LEN];
    panel_pending_t pending;
    int64_t fetched_at_ms;
    uint32_t selection_epoch;
    bool valid;
    bool gone;                      /* gateway 404: the session has ended */
} panel_detail_t;

/* ---- action command / result ----------------------------------------- */

typedef struct {
    panel_action_id_t action;
    char terminal_id[PANEL_TERM_ID_LEN];
    char context_token[PANEL_TOKEN_LEN];
    char prompt[PANEL_PROMPT_LEN];
    char request_id[PANEL_REQUEST_ID_LEN];
    uint32_t selection_epoch;
} panel_action_cmd_t;

typedef struct {
    char request_id[PANEL_REQUEST_ID_LEN];
    panel_action_state_t state;
    char message[PANEL_MESSAGE_LEN];
    uint32_t selection_epoch;
} panel_action_result_t;

/* ---- helpers ---------------------------------------------------------- */

static inline panel_agent_state_t panel_agent_state_from_str(const char *s)
{
    if (s == NULL) return PANEL_AGENT_UNKNOWN;
    if (strcmp(s, "working") == 0) return PANEL_AGENT_WORKING;
    if (strcmp(s, "blocked") == 0) return PANEL_AGENT_BLOCKED;
    if (strcmp(s, "done") == 0) return PANEL_AGENT_DONE;
    if (strcmp(s, "idle") == 0) return PANEL_AGENT_IDLE;
    return PANEL_AGENT_UNKNOWN;
}

static inline const char *panel_agent_state_name(panel_agent_state_t st)
{
    switch (st) {
    case PANEL_AGENT_WORKING: return "Working";
    case PANEL_AGENT_BLOCKED: return "Blocked";
    case PANEL_AGENT_DONE:    return "Done";
    case PANEL_AGENT_IDLE:    return "Idle";
    default:                  return "Unknown";
    }
}

/* UI_DESIGN.md palette */
static inline uint32_t panel_agent_state_color(panel_agent_state_t st)
{
    switch (st) {
    case PANEL_AGENT_BLOCKED: return 0xFF6B68;
    case PANEL_AGENT_WORKING: return 0xF7BD4A;
    case PANEL_AGENT_DONE:    return 0x69B5FF;
    case PANEL_AGENT_IDLE:    return 0x70C995;
    default:                  return 0x8D9BAA;
    }
}

static inline const char *panel_conn_state_name(panel_conn_state_t st)
{
    switch (st) {
    case PANEL_CONN_PROVISIONING:    return "Provisioning";
    case PANEL_CONN_WIFI_CONNECTING: return "Connecting Wi-Fi";
    case PANEL_CONN_GATEWAY_OFFLINE: return "Server offline";
    case PANEL_CONN_HERDR_DEGRADED:  return "Herdr degraded";
    case PANEL_CONN_ONLINE:          return "Online";
    default:                         return "Unknown";
    }
}

static inline const char *panel_action_state_name(panel_action_state_t st)
{
    switch (st) {
    case PANEL_ACTION_READY:       return "Ready";
    case PANEL_ACTION_CONFIRMING:  return "Confirming";
    case PANEL_ACTION_SENDING:     return "Sending...";
    case PANEL_ACTION_DELIVERED:   return "Delivered";
    case PANEL_ACTION_OBSERVED:    return "Done";
    case PANEL_ACTION_UNCERTAIN:   return "Unknown result";
    case PANEL_ACTION_STALE:       return "Stale";
    case PANEL_ACTION_UNSUPPORTED: return "Unsupported";
    default:                       return "No action";
    }
}

static inline const char *panel_action_id_name(panel_action_id_t a)
{
    switch (a) {
    case PANEL_ACT_ALLOW_ONCE:   return "Allow once";
    case PANEL_ACT_ALLOW_ALWAYS: return "Always allow";
    case PANEL_ACT_DENY:         return "Deny";
    case PANEL_ACT_CONTINUE:     return "Continue";
    default:                     return "";
    }
}

static inline panel_action_id_t panel_choice_to_action(uint8_t choice_bit)
{
    switch (choice_bit) {
    case PANEL_CHOICE_ALLOW_ONCE:   return PANEL_ACT_ALLOW_ONCE;
    case PANEL_CHOICE_ALLOW_ALWAYS: return PANEL_ACT_ALLOW_ALWAYS;
    case PANEL_CHOICE_DENY:         return PANEL_ACT_DENY;
    case PANEL_CHOICE_CONTINUE:     return PANEL_ACT_CONTINUE;
    default:                        return PANEL_ACT_NONE;
    }
}

/* Sanitize untrusted text for labels: strip ANSI/controls, keep UTF-8 bytes. */
static inline void panel_sanitize_text(char *s, size_t max_len)
{
    if (s == NULL || max_len == 0) return;
    size_t j = 0;
    for (size_t i = 0; s[i] != '\0' && j + 1 < max_len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1B) {
            /* skip CSI / simple ESC sequences */
            i++;
            if (s[i] == '[') {
                i++;
                while (s[i] != '\0' && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) i++;
            }
            continue;
        }
        if (c < 0x20 && c != '\n') continue;
        if (c == 0x7F) continue;
        s[j++] = (char)c;
    }
    s[j] = '\0';
}

#ifdef __cplusplus
}
#endif
