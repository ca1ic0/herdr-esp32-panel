/*
 * LVGL UI — Herdr decision panel (UI_DESIGN.md v1).
 *
 * HOM  4-grid overview, swipe pages
 * DET  session detail + pending card + action buttons (gateway choices only)
 * CNF  full-screen confirm before any semantic action
 * RST  sending / delivered / uncertain result
 * SET  connection, display, about, re-provision (view + simple toggles)
 * PRV  WPA2 provisioning QR
 *
 * Selection is (terminal_id, selection_epoch). Late network responses with
 * a mismatched epoch are discarded in panel_worker before reaching here.
 */

#include "ui_panel.h"

#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "panel_model.h"
#include "panel_store.h"
#include "ui_common.h"

#define SCREEN_W     480
#define SCREEN_H     480

/* UI_DESIGN.md §4.1 card slots */
static const struct { int x, y, w, h; } CARD_POS[4] = {
    { 16,  76, 218, 160 },
    { 246, 76, 218, 160 },
    { 16,  248, 218, 160 },
    { 246, 248, 218, 160 },
};

#define PAGE_SIZE    4

typedef enum {
    SCR_HOME = 0,
    SCR_DETAIL,
    SCR_CONFIRM,
    SCR_RESULT,
    SCR_SETTINGS,
    SCR_PROVISION,
} screen_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *shape;      /* status circle */
    lv_obj_t *shape_lbl;  /* ! / ? / check inside shape */
    lv_obj_t *name;
    lv_obj_t *status;
    lv_obj_t *project;
    lv_obj_t *pane;
    char terminal_id[PANEL_TERM_ID_LEN];
} cell_t;

/* ---- widget handles --------------------------------------------------- */

static lv_obj_t *s_home, *s_detail, *s_confirm, *s_result, *s_settings, *s_prov;

static lv_obj_t *s_conn_badge, *s_pending_count, *s_page_label, *s_total_label;
static lv_obj_t *s_empty_label;
static cell_t s_cells[4];

static lv_obj_t *s_det_name, *s_det_status, *s_det_project, *s_det_updated;
static lv_obj_t *s_det_content;
static lv_obj_t *s_btn_allow, *s_btn_deny, *s_btn_continue, *s_btn_host;
static lv_obj_t *s_det_hint;

static lv_obj_t *s_cnf_title, *s_cnf_body, *s_cnf_hint;
static lv_obj_t *s_cnf_cancel, *s_cnf_ok;

static lv_obj_t *s_rst_title, *s_rst_body, *s_rst_back;

static lv_obj_t *s_set_conn, *s_set_display, *s_set_about;

static lv_obj_t *s_prov_qr, *s_prov_info;

/* ---- local view state -------------------------------------------------- */

static panel_agent_card_t s_cards[PANEL_MAX_AGENTS];
static int s_card_count;
static int s_page;
static screen_t s_screen = SCR_HOME;

/* selection */
static char s_sel_term[PANEL_TERM_ID_LEN];
static uint32_t s_sel_epoch;
static panel_action_id_t s_pending_action;
static panel_pending_t s_frozen_pending;

/* display prefs (UI-local) */
static int s_brightness = 45;
static bool s_reduce_motion;

/* provisioning copy */
static char s_prov_ssid[24];
static char s_prov_pass[12];
static char s_prov_qr_text[128];

static uint32_t s_seen_generation;

/* ---- forward decls ----------------------------------------------------- */
static void show_screen(screen_t s);
static void on_card(lv_event_t *e);
static void on_menu(lv_event_t *e);
static void on_back(lv_event_t *e);
static void on_action_btn(lv_event_t *e);
static void on_confirm_cancel(lv_event_t *e);
static void on_confirm_ok(lv_event_t *e);
static void on_result_back(lv_event_t *e);
static void on_refresh(lv_event_t *e);

/* ---- helpers ----------------------------------------------------------- */

static void bump_epoch(void)
{
    s_sel_epoch++;
}

static bool selection_matches(const char *term, uint32_t epoch)
{
    return strcmp(s_sel_term, term) == 0 && s_sel_epoch == epoch;
}

static const char *shape_symbol(panel_agent_state_t st)
{
    switch (st) {
    case PANEL_AGENT_BLOCKED: return LV_SYMBOL_WARNING;
    case PANEL_AGENT_WORKING: return LV_SYMBOL_REFRESH;
    case PANEL_AGENT_DONE:    return LV_SYMBOL_OK;
    case PANEL_AGENT_IDLE:    return LV_SYMBOL_MINUS;
    default:                  return "?";
    }
}

static void style_card_base(lv_obj_t *card, bool blocked, bool selected)
{
    uint32_t border = COLOR_BORDER;
    int width = 1;
    if (blocked) {
        border = COLOR_PENDING;
        width = 2;
    } else if (selected) {
        border = COLOR_ACCENT;
        width = 2;
    }
    lv_obj_set_style_border_color(card, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(card, width, 0);
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

/* ====================================================================== */
/* HOM                                                                     */
/* ====================================================================== */

static void build_home(void)
{
    s_home = make_screen();

    /* top bar: HERDR | blocked N | menu */
    ui_label(s_home, 16, 20, 100, "HERDR", 24, COLOR_TEXT);

    s_conn_badge = ui_label(s_home, 120, 28, 140, "", 18, COLOR_UNKNOWN);

    s_pending_count = ui_label(s_home, 260, 24, 140, "Blocked 0", 20, COLOR_PENDING);
    lv_obj_add_flag(s_pending_count, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pending_count, on_refresh, LV_EVENT_CLICKED, NULL);

    lv_obj_t *menu = ui_button(s_home, 400, 12, 64, 48, LV_SYMBOL_SETTINGS, COLOR_CARD, 20);
    lv_obj_add_event_cb(menu, on_menu, LV_EVENT_CLICKED, NULL);

    /* 4 cards */
    for (int i = 0; i < 4; i++) {
        cell_t *c = &s_cells[i];
        c->card = lv_obj_create(s_home);
        lv_obj_set_pos(c->card, CARD_POS[i].x, CARD_POS[i].y);
        lv_obj_set_size(c->card, CARD_POS[i].w, CARD_POS[i].h);
        lv_obj_set_style_bg_color(c->card, lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c->card, 14, 0);
        lv_obj_set_style_pad_all(c->card, 0, 0);
        lv_obj_clear_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c->card, on_card, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        c->shape = lv_obj_create(c->card);
        lv_obj_set_pos(c->shape, 12, 14);
        lv_obj_set_size(c->shape, 28, 28);
        lv_obj_set_style_radius(c->shape, 14, 0);
        lv_obj_set_style_bg_opa(c->shape, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c->shape, 0, 0);
        lv_obj_clear_flag(c->shape, LV_OBJ_FLAG_SCROLLABLE);

        c->shape_lbl = lv_label_create(c->shape);
        lv_label_set_text(c->shape_lbl, "");
        lv_obj_set_style_text_font(c->shape_lbl, ui_font(14), 0);
        lv_obj_set_style_text_color(c->shape_lbl, lv_color_hex(COLOR_BG), 0);
        lv_obj_center(c->shape_lbl);

        c->name = ui_label(c->card, 48, 12, CARD_POS[i].w - 60, "---", 20, COLOR_TEXT);
        c->status = ui_label(c->card, 12, 52, CARD_POS[i].w - 24, "", 24, COLOR_UNKNOWN);
        c->project = ui_label(c->card, 12, 92, CARD_POS[i].w - 24, "", 18, COLOR_DIM);
        c->pane = ui_label(c->card, 12, CARD_POS[i].h - 28, CARD_POS[i].w - 24, "", 12, COLOR_DISABLED);
    }

    /* empty state (0 sessions) */
    s_empty_label = ui_label(s_home, 40, 200, 400, "No active agents", 24, COLOR_DIM);
    lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    /* bottom bar */
    s_page_label = ui_label(s_home, 16, 436, 120, "Page 1/1", 18, COLOR_DIM);
    s_total_label = ui_label(s_home, 160, 436, 160, "Total 0", 18, COLOR_DIM);

    lv_obj_t *prev = ui_button(s_home, 340, 428, 52, 40, LV_SYMBOL_LEFT, COLOR_CARD, 18);
    lv_obj_t *next = ui_button(s_home, 404, 428, 52, 40, LV_SYMBOL_RIGHT, COLOR_CARD, 18);
    lv_obj_add_event_cb(prev, on_back, LV_EVENT_CLICKED, (void *)(intptr_t)100);
    lv_obj_add_event_cb(next, on_back, LV_EVENT_CLICKED, (void *)(intptr_t)101);
}

static void fill_cell(int slot, const panel_agent_card_t *a)
{
    cell_t *c = &s_cells[slot];
    if (a == NULL) {
        /* empty slot: dashed look via dim border, no click */
        lv_obj_set_style_border_color(c->card, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_border_width(c->card, 1, 0);
        lv_obj_set_style_bg_opa(c->card, LV_OPA_30, 0);
        lv_label_set_text(c->name, "");
        lv_label_set_text(c->status, "Empty");
        lv_label_set_text(c->project, "");
        lv_label_set_text(c->pane, "");
        lv_obj_set_style_bg_color(c->shape, lv_color_hex(COLOR_BORDER), 0);
        lv_label_set_text(c->shape_lbl, "");
        c->terminal_id[0] = '\0';
        return;
    }

    lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
    bool blocked = (a->herdr_status == PANEL_AGENT_BLOCKED);
    style_card_base(c->card, blocked, false);

    uint32_t col = panel_agent_state_color(a->herdr_status);
    lv_obj_set_style_bg_color(c->shape, lv_color_hex(col), 0);
    /* blocked = solid; idle = hollow ring */
    if (a->herdr_status == PANEL_AGENT_IDLE) {
        lv_obj_set_style_bg_opa(c->shape, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(c->shape, lv_color_hex(col), 0);
        lv_obj_set_style_border_width(c->shape, 3, 0);
    } else {
        lv_obj_set_style_bg_opa(c->shape, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c->shape, 0, 0);
    }
    lv_label_set_text(c->shape_lbl, shape_symbol(a->herdr_status));

    lv_label_set_text(c->name, a->display_name[0] ? a->display_name : a->agent);
    lv_label_set_text(c->status, panel_agent_state_name(a->herdr_status));
    lv_obj_set_style_text_color(c->status, lv_color_hex(col), 0);
    lv_label_set_text(c->project, a->cwd_tail[0] ? a->cwd_tail : a->workspace_label);
    lv_label_set_text(c->pane, a->pane_id);
    snprintf(c->terminal_id, sizeof(c->terminal_id), "%s", a->terminal_id);
}

static void refresh_home(const panel_overview_t *ov, panel_conn_state_t conn)
{
    lv_label_set_text(s_conn_badge, panel_conn_state_name(conn));
    lv_obj_set_style_text_color(s_conn_badge,
        lv_color_hex(conn == PANEL_CONN_ONLINE ? COLOR_IDLE : COLOR_PENDING), 0);

    int blocked_n = 0;
    for (int i = 0; i < ov->count; i++) {
        if (ov->agents[i].herdr_status == PANEL_AGENT_BLOCKED) blocked_n++;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "Blocked %d", blocked_n);
    lv_label_set_text(s_pending_count, buf);

    int pages = (ov->count + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 1) pages = 1;
    if (s_page >= pages) s_page = pages - 1;
    if (s_page < 0) s_page = 0;

    snprintf(buf, sizeof(buf), "Page %d/%d", s_page + 1, pages);
    lv_label_set_text(s_page_label, buf);
    snprintf(buf, sizeof(buf), "Total %d", ov->count);
    lv_label_set_text(s_total_label, buf);

    if (ov->count == 0) {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 4; i++) {
            lv_obj_add_flag(s_cells[i].card, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 4; i++) {
        int idx = s_page * PAGE_SIZE + i;
        lv_obj_clear_flag(s_cells[i].card, LV_OBJ_FLAG_HIDDEN);
        if (idx < ov->count) {
            fill_cell(i, &ov->agents[idx]);
        } else {
            fill_cell(i, NULL);
        }
    }
}

/* ====================================================================== */
/* DET                                                                     */
/* ====================================================================== */

static void build_detail(void)
{
    s_detail = make_screen();

    lv_obj_t *back = ui_button(s_detail, 8, 8, 56, 48, LV_SYMBOL_LEFT, COLOR_CARD, 22);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, (void *)(intptr_t)10);

    s_det_name = ui_label(s_detail, 72, 18, 280, "", 24, COLOR_TEXT);

    lv_obj_t *menu = ui_button(s_detail, 408, 8, 56, 48, "Refresh", COLOR_CARD, 18);
    lv_obj_add_event_cb(menu, on_refresh, LV_EVENT_CLICKED, NULL);

    s_det_status = ui_label(s_detail, 16, 72, 200, "", 24, COLOR_UNKNOWN);
    s_det_project = ui_label(s_detail, 16, 104, 280, "", 18, COLOR_DIM);
    s_det_updated = ui_label(s_detail, 300, 80, 164, "", 16, COLOR_DISABLED);

    s_det_content = lv_obj_create(s_detail);
    lv_obj_set_pos(s_det_content, 16, 140);
    lv_obj_set_size(s_det_content, 448, 176);
    lv_obj_set_style_bg_color(s_det_content, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_det_content, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_det_content, 12, 0);
    lv_obj_set_style_border_color(s_det_content, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_det_content, 1, 0);
    lv_obj_set_style_pad_all(s_det_content, 12, 0);

    /* action area: 16,332,448,88 — up to 2 buttons */
    s_btn_allow = ui_button(s_detail, 16, 332, 220, 88, "Allow once", COLOR_DONE, 22);
    s_btn_deny = ui_button(s_detail, 244, 332, 220, 88, "Deny", COLOR_PENDING, 22);
    s_btn_continue = ui_button(s_detail, 16, 332, 448, 88, "Continue", COLOR_ACCENT, 22);
    s_btn_host = ui_button(s_detail, 16, 332, 448, 88, "Use host terminal", COLOR_BORDER, 20);
    lv_obj_add_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_btn_allow, on_action_btn, LV_EVENT_CLICKED, (void *)(intptr_t)PANEL_ACT_ALLOW_ONCE);
    lv_obj_add_event_cb(s_btn_deny, on_action_btn, LV_EVENT_CLICKED, (void *)(intptr_t)PANEL_ACT_DENY);
    lv_obj_add_event_cb(s_btn_continue, on_action_btn, LV_EVENT_CLICKED, (void *)(intptr_t)PANEL_ACT_CONTINUE);
    lv_obj_add_event_cb(s_btn_host, on_refresh, LV_EVENT_CLICKED, NULL);

    s_det_hint = ui_label(s_detail, 16, 432, 448, "Confirm after action", 18, COLOR_DIM);
}

static void render_detail(const panel_detail_t *det, panel_conn_state_t conn)
{
    lv_label_set_text(s_det_name, det->agent[0] ? det->agent : det->terminal_id);

    uint32_t col = panel_agent_state_color(det->herdr_status);
    lv_label_set_text(s_det_status, panel_agent_state_name(det->herdr_status));
    lv_obj_set_style_text_color(s_det_status, lv_color_hex(col), 0);

    lv_label_set_text(s_det_project, det->pane_id);

    int age_s = 0;
    if (det->fetched_at_ms > 0) {
        /* monotonic age is computed in the tick from a shared clock */
    }
    char ubuf[48];
    snprintf(ubuf, sizeof(ubuf), "Updated %d s ago", age_s);
    lv_label_set_text(s_det_updated, ubuf);

    /* content: pending card first, else output lines */
    lv_obj_clean(s_det_content);
    if (det->pending.kind != PANEL_PENDING_NONE && det->pending.summary[0] != '\0') {
        lv_obj_t *sum = lv_label_create(s_det_content);
        lv_label_set_long_mode(sum, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sum, 424);
        lv_label_set_text(sum, det->pending.summary);
        lv_obj_set_style_text_font(sum, ui_font(20), 0);
        lv_obj_set_style_text_color(sum, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_pos(sum, 0, 0);

        if (det->pending.impact[0] != '\0') {
            lv_obj_t *imp = lv_label_create(s_det_content);
            lv_label_set_long_mode(imp, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(imp, 424);
            lv_label_set_text(imp, det->pending.impact);
            lv_obj_set_style_text_font(imp, ui_font(18), 0);
            lv_obj_set_style_text_color(imp, lv_color_hex(COLOR_DIM), 0);
            lv_obj_set_pos(imp, 0, 72);
        }

        if (det->pending.kind == PANEL_PENDING_CONTINUATION &&
            det->pending.prompt[0] != '\0') {
            lv_obj_t *pr = lv_label_create(s_det_content);
            lv_label_set_long_mode(pr, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(pr, 424);
            lv_label_set_text(pr, det->pending.prompt);
            lv_obj_set_style_text_font(pr, ui_font(18), 0);
            lv_obj_set_style_text_color(pr, lv_color_hex(COLOR_ACCENT), 0);
            lv_obj_set_pos(pr, 0, 120);
        }
    } else {
        for (int i = 0; i < det->output_line_count; i++) {
            lv_obj_t *ln = lv_label_create(s_det_content);
            lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
            lv_obj_set_width(ln, 424);
            lv_label_set_text(ln, det->output_lines[i]);
            lv_obj_set_style_text_font(ln, ui_font(16), 0);
            lv_obj_set_style_text_color(ln, lv_color_hex(COLOR_DIM), 0);
            lv_obj_set_pos(ln, 0, i * 20);
        }
        if (det->output_line_count == 0) {
            ui_label(s_det_content, 0, 60, 424, "(no output)", 18, COLOR_DISABLED);
        }
    }

    /* action visibility from gateway choices only */
    bool online = (conn == PANEL_CONN_ONLINE);
    bool fresh = det->valid && online;
    uint8_t choices = fresh ? det->pending.choices : 0;

    lv_obj_add_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);

    if (panel_store_action_reserved()) {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "Action in progress");
        lv_label_set_text(s_det_hint, "Wait for current action");
        return;
    }

    if (det->pending.kind == PANEL_PENDING_APPROVAL ||
        det->pending.kind == PANEL_PENDING_QUESTION) {
        bool has_allow = (choices & PANEL_CHOICE_ALLOW_ONCE) != 0;
        bool has_deny = (choices & PANEL_CHOICE_DENY) != 0;
        if (has_allow && has_deny) {
            lv_obj_clear_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_det_hint, "Confirm after action");
        } else if (has_allow) {
            lv_obj_clear_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_det_hint, "Confirm after action");
        } else if (has_deny) {
            lv_obj_clear_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_det_hint, "Confirm after action");
        } else {
            lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "Use host terminal");
            lv_label_set_text(s_det_hint, "Impact/options incomplete");
        }
    } else if ((det->herdr_status == PANEL_AGENT_IDLE ||
                det->herdr_status == PANEL_AGENT_DONE) &&
               (choices & PANEL_CHOICE_CONTINUE)) {
        lv_obj_clear_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_det_hint, "Confirm shows full prompt");
    } else if (det->pending.kind == PANEL_PENDING_UNRECOGNIZED ||
               det->herdr_status == PANEL_AGENT_BLOCKED) {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "Use host terminal");
        lv_label_set_text(s_det_hint, "Pending card unrecognized");
    } else if (!online) {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "Offline, actions disabled");
        lv_label_set_text(s_det_hint, "Reconnect to act");
    } else {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "No action available");
        lv_label_set_text(s_det_hint, "Use host terminal");
    }
}

/* ====================================================================== */
/* CNF                                                                     */
/* ====================================================================== */

static void build_confirm(void)
{
    s_confirm = make_screen();

    lv_obj_t *back = ui_button(s_confirm, 8, 8, 56, 48, LV_SYMBOL_LEFT, COLOR_CARD, 22);
    lv_obj_add_event_cb(back, on_confirm_cancel, LV_EVENT_CLICKED, NULL);

    s_cnf_title = ui_label(s_confirm, 72, 18, 360, "Confirm", 26, COLOR_TEXT);

    ui_label(s_confirm, 16, 80, 448, "", 18, COLOR_DIM); /* spacer */

    s_cnf_body = lv_label_create(s_confirm);
    lv_label_set_long_mode(s_cnf_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_cnf_body, 16, 96);
    lv_obj_set_width(s_cnf_body, 448);
    lv_obj_set_height(s_cnf_body, 240);
    lv_obj_set_style_text_font(s_cnf_body, ui_font(20), 0);
    lv_obj_set_style_text_color(s_cnf_body, lv_color_hex(COLOR_TEXT), 0);

    s_cnf_hint = ui_label(s_confirm, 16, 348, 448, "Auto-cancel after 10 s", 16, COLOR_DISABLED);

    s_cnf_cancel = ui_button(s_confirm, 16, 372, 448, 48, "Cancel, back to detail", COLOR_CARD, 20);
    s_cnf_ok = ui_button(s_confirm, 16, 428, 448, 48, "Confirm", COLOR_ACCENT, 22);
    lv_obj_add_event_cb(s_cnf_cancel, on_confirm_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_cnf_ok, on_confirm_ok, LV_EVENT_CLICKED, NULL);
}

static void show_confirm(panel_action_id_t act, const panel_detail_t *det)
{
    s_pending_action = act;
    s_frozen_pending = det->pending;

    char body[512];
    if (act == PANEL_ACT_CONTINUE) {
        snprintf(body, sizeof(body),
                 "%s · %s\n\nPrompt to send:\n%s",
                 det->agent, det->pane_id,
                 det->pending.prompt[0] ? det->pending.prompt : "(empty)");
        lv_label_set_text(s_cnf_title, "Confirm continue");
    } else if (act == PANEL_ACT_ALLOW_ALWAYS) {
        snprintf(body, sizeof(body),
                 "%s · %s\n\nRequest: %s\nImpact: %s\n\nMay change future approvals",
                 det->agent, det->pane_id,
                 det->pending.summary, det->pending.impact);
        lv_label_set_text(s_cnf_title, "Confirm always allow");
    } else if (act == PANEL_ACT_DENY) {
        snprintf(body, sizeof(body),
                 "%s · %s\n\nDeny request: %s\nImpact: %s",
                 det->agent, det->pane_id,
                 det->pending.summary, det->pending.impact);
        lv_label_set_text(s_cnf_title, "Confirm deny");
    } else {
        snprintf(body, sizeof(body),
                 "%s · %s\n\nRequest: %s\nCwd/impact: %s\nScope: this only",
                 det->agent, det->pane_id,
                 det->pending.summary, det->pending.impact);
        lv_label_set_text(s_cnf_title, "Confirm allow once");
    }
    lv_label_set_text(s_cnf_body, body);
    lv_label_set_text(s_cnf_ok, panel_action_id_name(act));
    show_screen(SCR_CONFIRM);
}

/* ====================================================================== */
/* RST                                                                     */
/* ====================================================================== */

static void build_result(void)
{
    s_result = make_screen();
    s_rst_title = ui_label(s_result, 16, 80, 448, "", 28, COLOR_TEXT);
    lv_obj_set_style_text_align(s_rst_title, LV_TEXT_ALIGN_CENTER, 0);

    s_rst_body = ui_label(s_result, 16, 160, 448, "", 20, COLOR_DIM);
    lv_obj_set_style_text_align(s_rst_body, LV_TEXT_ALIGN_CENTER, 0);

    s_rst_back = ui_button(s_result, 16, 360, 448, 80, "Back to detail", COLOR_CARD, 22);
    lv_obj_add_event_cb(s_rst_back, on_result_back, LV_EVENT_CLICKED, NULL);
}

static void show_result(const panel_action_result_t *r)
{
    switch (r->state) {
    case PANEL_ACTION_SENDING:
        lv_label_set_text(s_rst_title, "Sending...");
        lv_label_set_text(s_rst_body, "Do not retry");
        break;
    case PANEL_ACTION_DELIVERED:
        lv_label_set_text(s_rst_title, "Delivered");
        lv_label_set_text(s_rst_body, "Waiting for agent");
        break;
    case PANEL_ACTION_OBSERVED:
        lv_label_set_text(s_rst_title, "Done");
        lv_label_set_text(s_rst_body, r->message[0] ? r->message : "Context changed (confirmed)");
        break;
    case PANEL_ACTION_STALE:
        lv_label_set_text(s_rst_title, "Context changed");
        lv_label_set_text(s_rst_body, "Prompt changed, review again");
        break;
    case PANEL_ACTION_UNSUPPORTED:
        lv_label_set_text(s_rst_title, "Unsupported");
        lv_label_set_text(s_rst_body, r->message[0] ? r->message : "Use host terminal");
        break;
    case PANEL_ACTION_UNAVAILABLE:
        lv_label_set_text(s_rst_title, "Auth/protocol error");
        lv_label_set_text(s_rst_body, r->message[0] ? r->message : "Update credentials");
        break;
    default:
        lv_label_set_text(s_rst_title, "Result unknown");
        lv_label_set_text(s_rst_body, "Check host; no auto-retry");
        break;
    }
    show_screen(SCR_RESULT);
}

/* ====================================================================== */
/* SET                                                                     */
/* ====================================================================== */

static void build_settings(void)
{
    s_settings = make_screen();

    lv_obj_t *back = ui_button(s_settings, 8, 8, 56, 48, LV_SYMBOL_LEFT, COLOR_CARD, 22);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, (void *)(intptr_t)11);
    ui_label(s_settings, 72, 22, 300, "Settings", 24, COLOR_TEXT);

    /* 4 rows ≥84 px — view only, no virtual keyboard */
    s_set_conn = lv_obj_create(s_settings);
    lv_obj_set_pos(s_set_conn, 16, 80);
    lv_obj_set_size(s_set_conn, 448, 100);
    lv_obj_set_style_bg_color(s_set_conn, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_set_conn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_set_conn, 12, 0);
    ui_label(s_set_conn, 12, 12, 400, "Connection", 20, COLOR_TEXT);
    ui_label(s_set_conn, 12, 44, 400, "", 16, COLOR_DIM);

    s_set_display = lv_obj_create(s_settings);
    lv_obj_set_pos(s_set_display, 16, 196);
    lv_obj_set_size(s_set_display, 448, 100);
    lv_obj_set_style_bg_color(s_set_display, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_set_display, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_set_display, 12, 0);
    ui_label(s_set_display, 12, 12, 400, "Display", 20, COLOR_TEXT);
    ui_label(s_set_display, 12, 44, 400, "", 16, COLOR_DIM);

    s_set_about = lv_obj_create(s_settings);
    lv_obj_set_pos(s_set_about, 16, 312);
    lv_obj_set_size(s_set_about, 448, 100);
    lv_obj_set_style_bg_color(s_set_about, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_set_about, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_set_about, 12, 0);
    ui_label(s_set_about, 12, 12, 400, "About", 20, COLOR_TEXT);
    ui_label(s_set_about, 12, 44, 400, "", 16, COLOR_DIM);

    /* re-provision entry */
    lv_obj_t *re = ui_button(s_settings, 16, 428, 448, 44, "Re-provision", COLOR_PENDING, 20);
    lv_obj_add_event_cb(re, on_menu, LV_EVENT_CLICKED, (void *)(intptr_t)3);
}

static void refresh_settings(const panel_overview_t *ov, panel_conn_state_t conn)
{
    app_config_t cfg;
    app_config_get(&cfg);

    char buf[200];
    /* Never render password/token. */
    snprintf(buf, sizeof(buf), "Wi-Fi: %s\nServer: %s:%u · %s",
             cfg.wifi_ssid, cfg.backend_host, (unsigned)cfg.backend_port,
             panel_conn_state_name(conn));
    lv_label_set_text(lv_obj_get_child(s_set_conn, 1), buf);

    snprintf(buf, sizeof(buf), "Bright %d%% · Reduce motion %s",
             s_brightness, s_reduce_motion ? "On" : "Off");
    lv_label_set_text(lv_obj_get_child(s_set_display, 1), buf);

    snprintf(buf, sizeof(buf), "FW v0.2 · Proto v1 · Sessions %d", ov->count);
    lv_label_set_text(lv_obj_get_child(s_set_about, 1), buf);
}

/* ====================================================================== */
/* PRV                                                                     */
/* ====================================================================== */

static void build_provision(void)
{
    s_prov = make_screen();
    ui_label(s_prov, 16, 24, 448, "Join device hotspot", 28, COLOR_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(s_prov, 0), LV_TEXT_ALIGN_CENTER, 0);

    /* white quiet zone */
    lv_obj_t *zone = lv_obj_create(s_prov);
    lv_obj_set_pos(zone, 112, 92);
    lv_obj_set_size(zone, 256, 256);
    lv_obj_set_style_bg_color(zone, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(zone, 8, 0);
    lv_obj_set_style_pad_all(zone, 12, 0);
    lv_obj_set_style_border_width(zone, 0, 0);

#if LV_USE_QRCODE
    s_prov_qr = lv_qrcode_create(zone);
    lv_qrcode_set_size(s_prov_qr, 232);
    lv_qrcode_set_dark_color(s_prov_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_prov_qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(s_prov_qr, true);
    lv_obj_center(s_prov_qr);
#else
    s_prov_qr = NULL;
    ui_label(zone, 20, 100, 216, "QR", 24, 0x000000);
#endif

    s_prov_info = ui_label(s_prov, 16, 360, 448, "", 18, COLOR_TEXT);
    lv_obj_set_style_text_align(s_prov_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_prov_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_prov_info, 100);
}

static void show_provision_screen(const char *ssid, const char *pass, const char *qr)
{
    snprintf(s_prov_ssid, sizeof(s_prov_ssid), "%s", ssid ? ssid : "");
    snprintf(s_prov_pass, sizeof(s_prov_pass), "%s", pass ? pass : "");
    snprintf(s_prov_qr_text, sizeof(s_prov_qr_text), "%s", qr ? qr : "");

#if LV_USE_QRCODE
    if (s_prov_qr != NULL) {
        lv_qrcode_update(s_prov_qr, s_prov_qr_text, strlen(s_prov_qr_text));
    }
#endif

    char info[192];
    snprintf(info, sizeof(info),
             "SSID: %s\nPass: %s\nScan to join Wi-Fi\nNo page? 192.168.4.1",
             s_prov_ssid, s_prov_pass);
    lv_label_set_text(s_prov_info, info);
    show_screen(SCR_PROVISION);
}

/* ====================================================================== */
/* screen switching / events                                              */
/* ====================================================================== */

static void show_screen(screen_t s)
{
    s_screen = s;
    lv_obj_t *scr = NULL;
    switch (s) {
    case SCR_HOME: scr = s_home; break;
    case SCR_DETAIL: scr = s_detail; break;
    case SCR_CONFIRM: scr = s_confirm; break;
    case SCR_RESULT: scr = s_result; break;
    case SCR_SETTINGS: scr = s_settings; break;
    case SCR_PROVISION: scr = s_prov; break;
    }
    if (scr != NULL) lv_screen_load(scr);
}

static void on_card(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= 4) return;
    const char *term = s_cells[slot].terminal_id;
    if (term[0] == '\0') return;

    bump_epoch();
    snprintf(s_sel_term, sizeof(s_sel_term), "%s", term);

    panel_cmd_t cmd = {
        .type = PANEL_CMD_OPEN_DETAIL,
        .selection_epoch = s_sel_epoch,
    };
    snprintf(cmd.terminal_id, sizeof(cmd.terminal_id), "%s", term);
    if (!panel_store_enqueue_control(&cmd)) {
        /* queue full: non-blocking, show busy */
        return;
    }
    show_screen(SCR_DETAIL);
}

static void on_menu(lv_event_t *e)
{
    void *ud = lv_event_get_user_data(e);
    if (ud == (void *)(intptr_t)3) {
        /* re-provision: clear creds + restart into SoftAP */
        app_config_clear_credentials();
        esp_restart();
        return;
    }
    if (s_screen == SCR_HOME) {
        panel_overview_t ov;
        panel_conn_state_t conn;
        panel_store_get_overview(&ov, &conn);
        refresh_settings(&ov, conn);
        show_screen(SCR_SETTINGS);
    }
}

static void on_back(lv_event_t *e)
{
    void *ud = lv_event_get_user_data(e);
    intptr_t code = (intptr_t)ud;

    if (code == 100) { /* prev page */
        if (s_page > 0) s_page--;
        return;
    }
    if (code == 101) { /* next page */
        int pages = (s_card_count + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages < 1) pages = 1;
        if (s_page < pages - 1) s_page++;
        return;
    }

    if (s_screen == SCR_DETAIL) {
        bump_epoch();
        s_sel_term[0] = '\0';
        show_screen(SCR_HOME);
    } else if (s_screen == SCR_SETTINGS || s_screen == SCR_RESULT) {
        show_screen(SCR_HOME);
    } else if (s_screen == SCR_CONFIRM) {
        s_pending_action = PANEL_ACT_NONE;
        show_screen(SCR_DETAIL);
    }
}

static void on_refresh(lv_event_t *e)
{
    (void)e;
    panel_cmd_t cmd = { .type = PANEL_CMD_REFRESH };
    panel_store_enqueue_control(&cmd);
}

static void on_action_btn(lv_event_t *e)
{
    panel_action_id_t act = (panel_action_id_t)(intptr_t)lv_event_get_user_data(e);
    panel_detail_t det;
    if (!panel_store_get_detail(&det)) return;
    if (!selection_matches(det.terminal_id, det.selection_epoch) &&
        strcmp(s_sel_term, det.terminal_id) != 0) {
        return;
    }
    /* freeze card; confirm page shows impact */
    show_confirm(act, &det);
}

static void on_confirm_cancel(lv_event_t *e)
{
    (void)e;
    s_pending_action = PANEL_ACT_NONE;
    show_screen(SCR_DETAIL);
}

static void on_confirm_ok(lv_event_t *e)
{
    (void)e;
    if (s_pending_action == PANEL_ACT_NONE) return;
    if (panel_store_action_reserved()) {
        show_result(&(panel_action_result_t){
            .state = PANEL_ACTION_SENDING,
        });
        return;
    }
    if (!panel_store_try_reserve_action()) {
        lv_label_set_text(s_cnf_hint, "Busy, try later");
        return;
    }

    panel_detail_t det;
    if (!panel_store_get_detail(&det)) {
        panel_store_release_action();
        return;
    }

    panel_cmd_t cmd = {
        .type = PANEL_CMD_ACTION,
        .action = s_pending_action,
        .selection_epoch = s_sel_epoch,
    };
    snprintf(cmd.terminal_id, sizeof(cmd.terminal_id), "%s", s_sel_term);
    snprintf(cmd.context_token, sizeof(cmd.context_token), "%s", s_frozen_pending.context_token);
    if (s_pending_action == PANEL_ACT_CONTINUE) {
        snprintf(cmd.prompt, sizeof(cmd.prompt), "%s", s_frozen_pending.prompt);
    }

    if (!panel_store_enqueue_action(&cmd)) {
        panel_store_release_action();
        lv_label_set_text(s_cnf_hint, "Busy, try later");
        return;
    }

    s_pending_action = PANEL_ACT_NONE;
    panel_action_result_t sending = { .state = PANEL_ACTION_SENDING };
    snprintf(sending.message, sizeof(sending.message), "Sending...");
    show_result(&sending);
}

static void on_result_back(lv_event_t *e)
{
    (void)e;
    if (s_sel_term[0] != '\0') {
        show_screen(SCR_DETAIL);
    } else {
        show_screen(SCR_HOME);
    }
}

/* ====================================================================== */
/* public                                                                  */
/* ====================================================================== */

void ui_panel_init(void)
{
    build_home();
    build_detail();
    build_confirm();
    build_result();
    build_settings();
    build_provision();
    show_screen(SCR_HOME);
}

void ui_show_provisioning(const char *ap_ssid, const char *ap_pass, const char *qr_payload)
{
    show_provision_screen(ap_ssid, ap_pass, qr_payload);
}

void ui_panel_tick(void)
{
    /* 1. drain one-shot UI events (action results etc.) */
    panel_ui_evt_t evt;
    while (panel_store_recv_ui_event(&evt, 0)) {
        if (evt.type == PANEL_EVT_ACTION_RESULT) {
            if (s_screen == SCR_CONFIRM || s_screen == SCR_RESULT) {
                show_result(&evt.action);
            }
        }
    }

    /* 2. redraw when store generation changed */
    uint32_t gen = panel_store_generation();
    if (gen == s_seen_generation) return;
    s_seen_generation = gen;

    panel_overview_t ov;
    panel_conn_state_t conn;
    panel_store_get_overview(&ov, &conn);
    s_card_count = ov.count;

    if (s_screen == SCR_HOME) {
        refresh_home(&ov, conn);
    } else if (s_screen == SCR_DETAIL) {
        panel_detail_t det;
        if (panel_store_get_detail(&det)) {
            if (strcmp(det.terminal_id, s_sel_term) == 0) {
                render_detail(&det, conn);
            }
        }
    } else if (s_screen == SCR_SETTINGS) {
        refresh_settings(&ov, conn);
    }
}
