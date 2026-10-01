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
#include <time.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "panel_model.h"
#include "panel_store.h"
#include "panel_prefs.h"
#include "panel_display.h"
#include "panel_audio.h"
#include "panel_power.h"
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
    SCR_BOOT = 0,
    SCR_HOME,
    SCR_DETAIL,
    SCR_CONFIRM,
    SCR_RESULT,
    SCR_SETTINGS,
    SCR_QUICK,
    SCR_SOUND,
    SCR_SOUND_MORE,
    SCR_QUIET,
    SCR_DISPLAY,
    SCR_SESSIONS,
    SCR_CONNECTION,
    SCR_ABOUT,
    SCR_REPROVISION_CONFIRM,
    SCR_PROVISION,
} screen_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *shape;      /* status circle */
    lv_obj_t *shape_lbl;  /* ! / ? / check inside shape */
    lv_obj_t *name;       /* workspace / project (primary) */
    lv_obj_t *status;
    lv_obj_t *icon;       /* 8-bit agent harness icon */
    lv_obj_t *agent;      /* agent kind, small dim text */
    lv_obj_t *pane;
    char terminal_id[PANEL_TERM_ID_LEN];
    panel_agent_state_t last_status;
    int64_t alert_start_ms;
} cell_t;

/* ---- widget handles --------------------------------------------------- */

static lv_obj_t *s_boot, *s_home, *s_detail, *s_confirm, *s_result, *s_settings, *s_prov;
static lv_obj_t *s_boot_cells[4], *s_boot_progress;
static lv_obj_t *s_quick, *s_sound, *s_sound_more, *s_quiet;
static lv_obj_t *s_display, *s_sessions, *s_connection, *s_about, *s_reprovision;

static lv_obj_t *s_conn_badge, *s_pending_count, *s_page_label, *s_total_label;
static lv_obj_t *s_battery_label, *s_battery_fill, *s_connection_power;
static lv_obj_t *s_sound_notice;
static lv_obj_t *s_empty_label;
static cell_t s_cells[4];

static lv_obj_t *s_det_name, *s_det_status, *s_det_project, *s_det_updated;
static lv_obj_t *s_det_content;
static lv_obj_t *s_btn_allow, *s_btn_deny, *s_btn_always, *s_btn_continue, *s_btn_host;
static lv_obj_t *s_det_hint;

static lv_obj_t *s_cnf_title, *s_cnf_body, *s_cnf_hint;
static lv_obj_t *s_cnf_cancel, *s_cnf_ok;

static lv_obj_t *s_rst_title, *s_rst_body, *s_rst_back;

static lv_obj_t *s_quick_brightness, *s_display_brightness, *s_sound_volume;
static lv_obj_t *s_sound_status;
static lv_obj_t *s_quiet_note;
typedef struct {
    lv_obj_t *button;
    panel_pref_field_t field;
    const char *name;
} pref_widget_t;
static pref_widget_t s_pref_widgets[32];
static int s_pref_widget_count;
static lv_obj_t *s_pref_status[9];
static int s_pref_status_count;

static lv_obj_t *s_prov_qr, *s_prov_info;

/* ---- local view state -------------------------------------------------- */

static int s_card_count;
static int s_page;
static screen_t s_screen = SCR_HOME;

/* selection */
static char s_sel_term[PANEL_TERM_ID_LEN];
static uint32_t s_sel_epoch;
static panel_action_id_t s_pending_action;
static panel_pending_t s_frozen_pending;
static int64_t s_confirm_started_ms;
static bool s_dimmed;
static int64_t s_boot_started_ms;
static bool s_boot_provision;
static bool s_boot_ready;

static uint32_t s_edit_id;

/* provisioning copy */
static char s_prov_ssid[24];
static char s_prov_pass[12];
static char s_prov_qr_text[128];

static uint32_t s_seen_generation;
static int s_last_age_sec = -1;
static bool s_prev_fresh;

static int64_t ui_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---- forward decls ----------------------------------------------------- */
static void show_screen(screen_t s);
static void show_provision_screen(const char *ssid, const char *pass, const char *qr);
static void on_card(lv_event_t *e);
static void on_menu(lv_event_t *e);
static void on_back(lv_event_t *e);
static void on_action_btn(lv_event_t *e);
static void on_confirm_cancel(lv_event_t *e);
static void on_confirm_ok(lv_event_t *e);
static void on_result_back(lv_event_t *e);
static void on_refresh(lv_event_t *e);
static void on_jump_blocked(lv_event_t *e);
static void on_home_gesture(lv_event_t *e);
static void refresh_prefs_ui(void);
static void on_pref_click(lv_event_t *e);
static void on_pref_slider(lv_event_t *e);
static void on_settings_nav(lv_event_t *e);
static void on_sound_test(lv_event_t *e);

static void refresh_power_ui(void)
{
    panel_power_status_t p;
    bool valid = panel_power_get(&p);
    char short_text[20];
    char detail[96];
    int percent = valid && p.battery_present ? p.percent : -1;
    if (!valid) {
        snprintf(short_text, sizeof(short_text), "%s", "--%");
        snprintf(detail, sizeof(detail), "%s", "电量：读取中");
    } else if (!p.battery_present) {
        snprintf(short_text, sizeof(short_text), "%s", p.external_power ? "USB" : "--%");
        snprintf(detail, sizeof(detail), "%s", p.external_power ?
                 "电池未连接 · USB 供电" : "电池状态不可用");
    } else if (percent < 0) {
        snprintf(short_text, sizeof(short_text), "%s", "--%");
        snprintf(detail, sizeof(detail), "%s", "电量暂不可读");
    } else {
        if (p.charging) snprintf(short_text, sizeof(short_text), "充%d%%", percent);
        else snprintf(short_text, sizeof(short_text), "%d%%", percent);
        snprintf(detail, sizeof(detail), "电量：%d%% · %s%s",
                 percent, p.charging ? "充电中" :
                 p.external_power ? "外接电源" : "电池供电",
                 p.millivolts > 0 ? "" : " · 电压未知");
    }
    static char last_text[20];
    static char last_detail[96];
    static int last_percent = -2;
    static bool last_charging;
    if (strcmp(short_text, last_text) == 0 &&
        strcmp(detail, last_detail) == 0 &&
        percent == last_percent &&
        (!valid || p.charging == last_charging)) return;
    snprintf(last_text, sizeof(last_text), "%s", short_text);
    snprintf(last_detail, sizeof(last_detail), "%s", detail);
    last_percent = percent;
    last_charging = valid && p.charging;
    if (s_battery_label) {
        lv_label_set_text(s_battery_label, short_text);
        lv_obj_set_style_text_color(s_battery_label,
            lv_color_hex(percent >= 0 && percent <= 15 && !p.charging ?
                         COLOR_PENDING : COLOR_DIM), 0);
    }
    if (s_battery_fill) {
        if (percent < 0) lv_obj_add_flag(s_battery_fill, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_clear_flag(s_battery_fill, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(s_battery_fill, percent > 0 ?
                             (percent * 56 + 99) / 100 : 1);
            lv_obj_set_style_bg_color(s_battery_fill,
                lv_color_hex(percent <= 15 && !p.charging ?
                             COLOR_PENDING : p.charging ? COLOR_IDLE : COLOR_ACCENT), 0);
        }
    }
    if (s_connection_power) lv_label_set_text(s_connection_power, detail);
}

/* ---- 8-bit style agent icons (16x16 1bpp pixel maps) -------------------- */

static const char *const ICON_CLAUDE[16] = {   /* spark */
    "................",
    ".......XX.......",
    ".......XX.......",
    ".......XX.......",
    "...XX..XX..XX...",
    "....XX.XX.XX....",
    ".....XXXXX......",
    ".XXXXXXXXXXXXX..",
    ".....XXXXX......",
    "....XX.XX.XX....",
    "...XX..XX..XX...",
    ".......XX.......",
    ".......XX.......",
    ".......XX.......",
    "................",
    "................",
};

static const char *const ICON_OPENCODE[16] = {  /* terminal prompt >_ */
    "................",
    "................",
    "..XX............",
    "...XX...........",
    "....XX..........",
    ".....XX.........",
    "....XX..........",
    "...XX...........",
    "..XX............",
    "................",
    "................",
    ".....XXXXXXXXX..",
    ".....XXXXXXXXX..",
    "................",
    "................",
    "................",
};

static const char *const ICON_PI[16] = {        /* π */
    "................",
    "................",
    "...XXXXXXXXXX...",
    "...XXXXXXXXXX...",
    "....XX....XX....",
    "....XX....XX....",
    "....XX....XX....",
    "....XX....XX....",
    "....XX....XX....",
    "....XX....XX....",
    "....XX....XX....",
    "...XX.....XX....",
    "...XX.....XXX...",
    "..XXX......XX...",
    "................",
    "................",
};

static const char *const ICON_UNKNOWN[16] = {   /* generic chip */
    "................",
    "................",
    "...XXXXXXXXXX...",
    "..XX.......XX...",
    "..X.XX...XX.X...",
    "..X.XX...XX.X...",
    "..X.........X...",
    "..X...XXX...X...",
    "..X...XXX...X...",
    "..X.........X...",
    "..XX.......XX...",
    "...XXXXXXXXXX...",
    "....XX...XX.....",
    "....XX...XX.....",
    "................",
    "................",
};

static lv_image_dsc_t s_icon_claude, s_icon_opencode, s_icon_pi, s_icon_unknown;
static uint8_t s_icon_data[4][16 * 16 * 4];  /* ARGB8888 */

static void build_icon(lv_image_dsc_t *dsc, uint8_t *buf,
                       const char *const rows[16], uint32_t rgb)
{
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            bool on = rows[y][x] == 'X';
            uint8_t *px = buf + (y * 16 + x) * 4;
            px[0] = (uint8_t)(rgb & 0xFF);          /* B */
            px[1] = (uint8_t)((rgb >> 8) & 0xFF);   /* G */
            px[2] = (uint8_t)((rgb >> 16) & 0xFF);  /* R */
            px[3] = on ? 0xFF : 0x00;               /* A */
        }
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.stride = 16 * 4;
    dsc->header.w = 16;
    dsc->header.h = 16;
    dsc->data = buf;
    dsc->data_size = 16 * 16 * 4;
}

static void build_agent_icons(void)
{
    build_icon(&s_icon_claude, s_icon_data[0], ICON_CLAUDE, 0xD97757);
    build_icon(&s_icon_opencode, s_icon_data[1], ICON_OPENCODE, 0x69B5FF);
    build_icon(&s_icon_pi, s_icon_data[2], ICON_PI, 0x70C995);
    build_icon(&s_icon_unknown, s_icon_data[3], ICON_UNKNOWN, 0x8D9BAA);
}

static const lv_image_dsc_t *agent_icon(const char *agent)
{
    if (strcmp(agent, "claude") == 0) return &s_icon_claude;
    if (strcmp(agent, "opencode") == 0) return &s_icon_opencode;
    if (strcmp(agent, "pi") == 0) return &s_icon_pi;
    return &s_icon_unknown;
}

/* ---- helpers ----------------------------------------------------------- */

static void bump_epoch(void)
{
    s_sel_epoch++;
}

/* Let LV_EVENT_GESTURE bubble from this object and all descendants, so a
 * swipe that starts on a card or label still reaches the home screen. */
static void bubble_gestures(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if (child != NULL) bubble_gestures(child);
    }
}

static void page_prev(void)
{
    if (s_page > 0) {
        s_page--;
        panel_store_set_view_context(s_page, s_sel_term);
        s_seen_generation = 0;  /* force redraw on next tick */
    }
}

static void page_next(void)
{
    int pages = (s_card_count + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 1) pages = 1;
    if (s_page < pages - 1) {
        s_page++;
        panel_store_set_view_context(s_page, s_sel_term);
        s_seen_generation = 0;  /* force redraw on next tick */
    }
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

static void build_boot(void)
{
    s_boot = make_screen();
    lv_obj_t *brand = ui_label(s_boot, 40, 98, 400, "HERDR", 28, COLOR_TEXT);
    lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < 4; i++) {
        s_boot_cells[i] = lv_obj_create(s_boot);
        lv_obj_set_pos(s_boot_cells[i], 204 + (i % 2) * 40,
                       177 + (i / 2) * 40);
        lv_obj_set_size(s_boot_cells[i], 28, 28);
        lv_obj_set_style_radius(s_boot_cells[i], 7, 0);
        lv_obj_set_style_bg_color(s_boot_cells[i], lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_bg_opa(s_boot_cells[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_boot_cells[i], 0, 0);
        lv_obj_clear_flag(s_boot_cells[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *subtitle = ui_label(s_boot, 40, 284, 400,
                                  "连接你的 Herdr 会话", 20, COLOR_DIM);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *track = lv_obj_create(s_boot);
    lv_obj_set_pos(track, 140, 346);
    lv_obj_set_size(track, 200, 4);
    lv_obj_set_style_bg_color(track, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    s_boot_progress = lv_obj_create(track);
    lv_obj_set_pos(s_boot_progress, 0, 0);
    lv_obj_set_size(s_boot_progress, 1, 4);
    lv_obj_set_style_bg_color(s_boot_progress, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_boot_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_boot_progress, 0, 0);
    lv_obj_set_style_pad_all(s_boot_progress, 0, 0);
}

static void tick_boot(void)
{
    panel_prefs_t p;
    panel_prefs_get(&p);
    int64_t elapsed = ui_now_ms() - s_boot_started_ms;
    int duration = p.reduce_motion ? 400 : 1200;
    if (elapsed >= duration && s_boot_ready) {
        if (s_boot_provision)
            show_screen(SCR_PROVISION);
        else
            show_screen(SCR_HOME);
        return;
    }
    int lit = p.reduce_motion ? 4 : (int)(elapsed * 4 / 900);
    if (lit > 4) lit = 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_boot_cells[i],
            lv_color_hex(i < lit ? COLOR_ACCENT : COLOR_BORDER), 0);
    }
    int progress = elapsed >= duration ? 200 :
                   p.reduce_motion ? 200 : 1 + (int)(elapsed * 199 / duration);
    lv_obj_set_width(s_boot_progress, progress);
}

/* ====================================================================== */
/* HOM                                                                     */
/* ====================================================================== */

static void build_home(void)
{
    s_home = make_screen();

    /* top bar: HERDR | blocked N | menu */
    ui_label(s_home, 16, 20, 100, "HERDR", 24, COLOR_TEXT);

    s_conn_badge = ui_label(s_home, 116, 28, 116, "", 18, COLOR_UNKNOWN);

    s_pending_count = ui_label(s_home, 236, 24, 102, "待处理 0", 18, COLOR_PENDING);
    lv_obj_add_flag(s_pending_count, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pending_count, on_jump_blocked, LV_EVENT_CLICKED, NULL);

    s_battery_label = ui_label(s_home, 340, 22, 56, "--%", 14, COLOR_DIM);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *battery_track = lv_obj_create(s_home);
    lv_obj_set_pos(battery_track, 340, 49);
    lv_obj_set_size(battery_track, 56, 4);
    lv_obj_set_style_bg_color(battery_track, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_opa(battery_track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_track, 0, 0);
    lv_obj_set_style_pad_all(battery_track, 0, 0);
    lv_obj_clear_flag(battery_track, LV_OBJ_FLAG_SCROLLABLE);
    s_battery_fill = lv_obj_create(battery_track);
    lv_obj_set_pos(s_battery_fill, 0, 0);
    lv_obj_set_size(s_battery_fill, 1, 4);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_battery_fill, 0, 0);
    lv_obj_set_style_pad_all(s_battery_fill, 0, 0);
    lv_obj_add_flag(s_battery_fill, LV_OBJ_FLAG_HIDDEN);

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

        c->icon = lv_image_create(c->card);
        lv_obj_set_pos(c->icon, 12, 86);
        lv_image_set_scale(c->icon, 512);   /* 2x nearest: keeps the 8-bit look */

        c->agent = ui_label(c->card, 54, 94, CARD_POS[i].w - 66, "", 14, COLOR_DIM);
        c->pane = ui_label(c->card, 12, CARD_POS[i].h - 28, CARD_POS[i].w - 24, "", 12, COLOR_DISABLED);
    }

    /* empty state (0 sessions) */
    s_empty_label = ui_label(s_home, 40, 200, 400, "暂无活跃会话", 24, COLOR_DIM);
    lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    /* bottom bar: page indicator + swipe hint (no buttons, gesture nav) */
    s_sound_notice = ui_label(s_home, 16, 411, 448, "", 14, COLOR_PENDING);
    s_page_label = ui_label(s_home, 16, 436, 120, "第 1/1 页", 18, COLOR_DIM);
    s_total_label = ui_label(s_home, 160, 436, 160, "共 0 个", 18, COLOR_DIM);
    lv_obj_t *hint = ui_label(s_home, 330, 436, 134, LV_SYMBOL_LEFT " swipe " LV_SYMBOL_RIGHT,
                              16, COLOR_DISABLED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);

    /* touch navigation: swipe left/right to turn pages; let gestures that
     * start on any child widget bubble up to this screen */
    lv_obj_add_event_cb(s_home, on_home_gesture, LV_EVENT_GESTURE, NULL);
    bubble_gestures(s_home);
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
        lv_label_set_text(c->status, "空位");
        lv_label_set_text(c->agent, "");
        lv_label_set_text(c->pane, "");
        lv_obj_add_flag(c->icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(c->shape, lv_color_hex(COLOR_BORDER), 0);
        lv_label_set_text(c->shape_lbl, "");
        c->terminal_id[0] = '\0';
        c->last_status = PANEL_AGENT_UNKNOWN;
        c->alert_start_ms = 0;
        return;
    }

    lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
    bool blocked = (a->herdr_status == PANEL_AGENT_BLOCKED);
    if (strcmp(c->terminal_id, a->terminal_id) != 0) c->alert_start_ms = 0;
    if (strcmp(c->terminal_id, a->terminal_id) == 0 &&
        c->last_status != PANEL_AGENT_UNKNOWN &&
        c->last_status != PANEL_AGENT_BLOCKED && blocked) {
        panel_prefs_t p;
        panel_prefs_get(&p);
        if (p.visual_alert && !p.reduce_motion) c->alert_start_ms = ui_now_ms();
    }
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

    /* workspace/project is the primary identity; the agent harness is
     * shown as its 8-bit icon plus a small kind label */
    const char *primary = a->workspace_label[0] ? a->workspace_label
                          : a->cwd_tail[0]      ? a->cwd_tail
                                                : a->display_name;
    lv_label_set_text(c->name, primary);
    lv_label_set_text(c->status, panel_agent_state_name(a->herdr_status));
    lv_obj_set_style_text_color(c->status, lv_color_hex(col), 0);
    lv_obj_clear_flag(c->icon, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(c->icon, agent_icon(a->agent));
    lv_label_set_text(c->agent, a->agent);
    lv_label_set_text(c->pane, a->pane_id);
    snprintf(c->terminal_id, sizeof(c->terminal_id), "%s", a->terminal_id);
    c->last_status = a->herdr_status;
}

static void update_alert_pulses(void)
{
    panel_prefs_t p;
    panel_prefs_get(&p);
    for (int i = 0; i < 4; i++) {
        cell_t *c = &s_cells[i];
        if (c->alert_start_ms == 0) continue;
        int64_t age = ui_now_ms() - c->alert_start_ms;
        if (age >= 800 || c->last_status != PANEL_AGENT_BLOCKED ||
            p.reduce_motion || !p.visual_alert) {
            c->alert_start_ms = 0;
            lv_obj_set_style_border_width(c->card, 2, 0);
        } else {
            bool peak = (age < 200) || (age >= 400 && age < 600);
            lv_obj_set_style_border_width(c->card, peak ? 5 : 2, 0);
        }
    }
}

static void refresh_home(const panel_agent_card_t cards[4], int count,
                         int total, panel_conn_state_t conn)
{
    lv_label_set_text(s_conn_badge, panel_conn_state_name(conn));
    lv_obj_set_style_text_color(s_conn_badge,
        lv_color_hex(conn == PANEL_CONN_ONLINE ? COLOR_IDLE : COLOR_PENDING), 0);

    char buf[48];
    snprintf(buf, sizeof(buf), "待处理 %d", panel_store_blocked_count());
    lv_label_set_text(s_pending_count, buf);

    int pages = (count + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 1) pages = 1;

    snprintf(buf, sizeof(buf), "第 %d/%d 页", s_page + 1, pages);
    lv_label_set_text(s_page_label, buf);
    snprintf(buf, sizeof(buf), "共 %d 个", total);
    lv_label_set_text(s_total_label, buf);
    char notice[PANEL_MESSAGE_LEN];
    panel_store_get_sound_notice(notice, sizeof(notice));
    lv_label_set_text(s_sound_notice, notice);

    if (count == 0) {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 4; i++) {
            lv_obj_add_flag(s_cells[i].card, LV_OBJ_FLAG_HIDDEN);
            s_cells[i].terminal_id[0] = '\0';
            s_cells[i].last_status = PANEL_AGENT_UNKNOWN;
            s_cells[i].alert_start_ms = 0;
        }
        return;
    }
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 4; i++) {
        lv_obj_clear_flag(s_cells[i].card, LV_OBJ_FLAG_HIDDEN);
        if (cards[i].terminal_id[0] != '\0') {
            fill_cell(i, &cards[i]);
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

    lv_obj_t *menu = ui_button(s_detail, 408, 8, 56, 48, "刷新", COLOR_CARD, 18);
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

    /* action area: 16,332,448,88 — 2 buttons, or 3 when allow_always exists */
    s_btn_allow = ui_button(s_detail, 16, 332, 220, 88, "允许一次", COLOR_DONE, 22);
    s_btn_deny = ui_button(s_detail, 244, 332, 220, 88, "拒绝", COLOR_PENDING, 22);
    s_btn_always = ui_button(s_detail, 324, 332, 140, 88, "始终允许", COLOR_WORKING, 18);
    s_btn_continue = ui_button(s_detail, 16, 332, 448, 88, "继续", COLOR_ACCENT, 22);
    s_btn_host = ui_button(s_detail, 16, 332, 448, 88, "请在主机处理", COLOR_BORDER, 20);
    lv_obj_add_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_always, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_btn_allow, on_action_btn, LV_EVENT_CLICKED, (void *)(intptr_t)PANEL_ACT_ALLOW_ONCE);
    lv_obj_add_event_cb(s_btn_deny, on_action_btn, LV_EVENT_CLICKED, (void *)(intptr_t)PANEL_ACT_DENY);
    lv_obj_add_event_cb(s_btn_always, on_action_btn, LV_EVENT_CLICKED, (void *)(intptr_t)PANEL_ACT_ALLOW_ALWAYS);
    lv_obj_add_event_cb(s_btn_continue, on_action_btn, LV_EVENT_CLICKED, (void *)(intptr_t)PANEL_ACT_CONTINUE);
    lv_obj_add_event_cb(s_btn_host, on_refresh, LV_EVENT_CLICKED, NULL);

    s_det_hint = ui_label(s_detail, 16, 432, 448, "操作前请确认", 18, COLOR_DIM);
}

static void render_detail(const panel_detail_t *det, panel_conn_state_t conn,
                          int age_s)
{
    if (det->gone) {
        lv_label_set_text(s_det_name, det->terminal_id);
        lv_label_set_text(s_det_status, "已结束");
        lv_obj_set_style_text_color(s_det_status, lv_color_hex(COLOR_UNKNOWN), 0);
        lv_label_set_text(s_det_project, "");
        lv_label_set_text(s_det_updated, "");
        lv_obj_clean(s_det_content);
        ui_label(s_det_content, 0, 60, 424, "会话已结束，请返回列表",
                 18, COLOR_DISABLED);
        lv_obj_add_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_always, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_det_hint, "会话已结束");
        return;
    }

    lv_label_set_text(s_det_name, det->agent[0] ? det->agent : det->terminal_id);

    uint32_t col = panel_agent_state_color(det->herdr_status);
    lv_label_set_text(s_det_status, panel_agent_state_name(det->herdr_status));
    lv_obj_set_style_text_color(s_det_status, lv_color_hex(col), 0);

    lv_label_set_text(s_det_project, det->pane_id);

    char ubuf[48];
    if (age_s > 6) {
        snprintf(ubuf, sizeof(ubuf), "数据已过期 %d 秒", age_s);
    } else {
        snprintf(ubuf, sizeof(ubuf), "更新于 %d 秒前", age_s);
    }
    lv_label_set_text(s_det_updated, ubuf);
    lv_obj_set_style_text_color(s_det_updated,
        lv_color_hex(age_s > 6 ? COLOR_PENDING : COLOR_DISABLED), 0);

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
            det->pending.source == PANEL_SOURCE_NEW_PROMPT &&
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

    /* action visibility from gateway choices only; actions need fresh data:
     * spec §4.3 — >6 s marks stale, >10 s disables actions entirely */
    bool online = (conn == PANEL_CONN_ONLINE);
    bool fresh = det->valid && online && age_s <= 10;
    uint8_t choices = fresh ? det->pending.choices : 0;

    lv_obj_add_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_always, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);

    if (panel_store_action_reserved()) {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "操作进行中");
        lv_label_set_text(s_det_hint, "请等待当前操作");
        return;
    }

    if (online && age_s > 10) {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "数据已过期");
        lv_label_set_text(s_det_hint, "正在刷新，操作已禁用");
        return;
    }

    if (det->pending.kind == PANEL_PENDING_APPROVAL ||
        det->pending.kind == PANEL_PENDING_QUESTION) {
        bool has_allow = (choices & PANEL_CHOICE_ALLOW_ONCE) != 0;
        bool has_always = (choices & PANEL_CHOICE_ALLOW_ALWAYS) != 0;
        bool has_deny = (choices & PANEL_CHOICE_DENY) != 0;
        if (has_allow && has_deny && has_always) {
            /* 3-button layout: 146 / 146 / 140 */
            lv_obj_set_pos(s_btn_allow, 16, 332);
            lv_obj_set_size(s_btn_allow, 146, 88);
            lv_obj_set_pos(s_btn_deny, 170, 332);
            lv_obj_set_size(s_btn_deny, 146, 88);
            lv_obj_clear_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_btn_always, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_det_hint, "始终允许需再次确认");
        } else if (has_allow || has_deny) {
            /* 2-button layout */
            lv_obj_set_pos(s_btn_allow, 16, 332);
            lv_obj_set_size(s_btn_allow, 220, 88);
            lv_obj_set_pos(s_btn_deny, 244, 332);
            lv_obj_set_size(s_btn_deny, 220, 88);
            if (has_allow) lv_obj_clear_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
            if (has_deny) lv_obj_clear_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_det_hint, age_s > 6 ? "数据已过期" : "操作前请确认");
        } else {
            lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "请在主机处理");
            lv_label_set_text(s_det_hint, "影响或选项不完整");
        }
    } else if ((det->herdr_status == PANEL_AGENT_IDLE ||
                det->herdr_status == PANEL_AGENT_DONE) &&
               (choices & PANEL_CHOICE_CONTINUE)) {
        lv_obj_clear_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_det_hint, age_s > 6 ? "数据已过期" : "确认页展示完整提示");
    } else if (det->pending.kind == PANEL_PENDING_UNRECOGNIZED ||
               det->herdr_status == PANEL_AGENT_BLOCKED) {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "请在主机处理");
        lv_label_set_text(s_det_hint, "无法识别当前请求");
    } else if (!online) {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "离线，操作已禁用");
        lv_label_set_text(s_det_hint, "重连后才能操作");
    } else {
        lv_obj_clear_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_btn_host, 0), "暂无可用操作");
        lv_label_set_text(s_det_hint, "请在主机处理");
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

    s_cnf_title = ui_label(s_confirm, 72, 18, 360, "确认", 26, COLOR_TEXT);

    ui_label(s_confirm, 16, 80, 448, "", 18, COLOR_DIM); /* spacer */

    lv_obj_t *body_scroll = lv_obj_create(s_confirm);
    lv_obj_set_pos(body_scroll, 16, 88);
    lv_obj_set_size(body_scroll, 448, 248);
    lv_obj_set_style_bg_opa(body_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body_scroll, 0, 0);
    lv_obj_set_style_pad_all(body_scroll, 0, 0);
    lv_obj_set_scroll_dir(body_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    s_cnf_body = lv_label_create(body_scroll);
    lv_label_set_long_mode(s_cnf_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_cnf_body, 0, 0);
    lv_obj_set_width(s_cnf_body, 424);
    lv_obj_set_height(s_cnf_body, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_cnf_body, ui_font(20), 0);
    lv_obj_set_style_text_color(s_cnf_body, lv_color_hex(COLOR_TEXT), 0);

    s_cnf_hint = ui_label(s_confirm, 16, 348, 448, "可上滑看全文 · 10 秒后取消", 16, COLOR_DISABLED);

    s_cnf_cancel = ui_button(s_confirm, 16, 372, 448, 48, "取消，返回详情", COLOR_CARD, 20);
    s_cnf_ok = ui_button(s_confirm, 16, 428, 448, 48, "确认", COLOR_ACCENT, 22);
    lv_obj_add_event_cb(s_cnf_cancel, on_confirm_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_cnf_ok, on_confirm_ok, LV_EVENT_CLICKED, NULL);
}

static void show_confirm(panel_action_id_t act, const panel_detail_t *det)
{
    s_pending_action = act;
    s_frozen_pending = det->pending;
    s_confirm_started_ms = ui_now_ms();

    char body[512];
    if (act == PANEL_ACT_CONTINUE &&
        det->pending.source == PANEL_SOURCE_NEW_PROMPT) {
        snprintf(body, sizeof(body),
                 "%s · %s\n\n将发送的提示词：\n%s",
                 det->agent, det->pane_id,
                 det->pending.prompt[0] ? det->pending.prompt : "（空）");
        lv_label_set_text(s_cnf_title, "确认继续");
    } else if (act == PANEL_ACT_CONTINUE) {
        snprintf(body, sizeof(body),
                 "%s · %s\n\n将选择终端中的继续选项：\n%s\n影响：%s",
                 det->agent, det->pane_id,
                 det->pending.summary, det->pending.impact);
        lv_label_set_text(s_cnf_title, "确认继续");
    } else if (act == PANEL_ACT_ALLOW_ALWAYS) {
        snprintf(body, sizeof(body),
                 "%s · %s\n\n请求：%s\n影响：%s\n\n可能改变后续审批",
                 det->agent, det->pane_id,
                 det->pending.summary, det->pending.impact);
        lv_label_set_text(s_cnf_title, "确认始终允许");
    } else if (act == PANEL_ACT_DENY) {
        snprintf(body, sizeof(body),
                 "%s · %s\n\n拒绝请求：%s\n影响：%s",
                 det->agent, det->pane_id,
                 det->pending.summary, det->pending.impact);
        lv_label_set_text(s_cnf_title, "确认拒绝");
    } else {
        snprintf(body, sizeof(body),
                 "%s · %s\n\n请求：%s\n目录/影响：%s\n范围：仅本次",
                 det->agent, det->pane_id,
                 det->pending.summary, det->pending.impact);
        lv_label_set_text(s_cnf_title, "确认允许一次");
    }
    lv_label_set_text(s_cnf_body, body);
    lv_label_set_text(lv_obj_get_child(s_cnf_ok, 0), panel_action_id_name(act));
    lv_label_set_text(s_cnf_hint, "可上滑看全文 · 10 秒后取消");
    show_screen(SCR_CONFIRM);
}

static uint8_t action_choice(panel_action_id_t action)
{
    switch (action) {
    case PANEL_ACT_ALLOW_ONCE: return PANEL_CHOICE_ALLOW_ONCE;
    case PANEL_ACT_ALLOW_ALWAYS: return PANEL_CHOICE_ALLOW_ALWAYS;
    case PANEL_ACT_DENY: return PANEL_CHOICE_DENY;
    case PANEL_ACT_CONTINUE: return PANEL_CHOICE_CONTINUE;
    default: return 0;
    }
}

/* Revalidate the exact visible request immediately before enqueue. */
static bool confirmed_request_current(void)
{
    if (s_pending_action == PANEL_ACT_NONE ||
        ui_now_ms() - s_confirm_started_ms >= 10000 ||
        panel_store_conn_state() != PANEL_CONN_ONLINE) return false;
    panel_detail_t det;
    if (!panel_store_get_detail(&det) || det.gone || !det.valid ||
        det.selection_epoch != s_sel_epoch ||
        strcmp(det.terminal_id, s_sel_term) != 0 ||
        det.fetched_at_ms <= 0 ||
        ui_now_ms() - det.fetched_at_ms > 10000 ||
        det.pending.kind != s_frozen_pending.kind ||
        det.pending.source != s_frozen_pending.source ||
        !(det.pending.choices & action_choice(s_pending_action)) ||
        strcmp(det.pending.context_token, s_frozen_pending.context_token) != 0)
        return false;
    if (s_pending_action == PANEL_ACT_CONTINUE &&
        strcmp(det.pending.prompt, s_frozen_pending.prompt) != 0) return false;
    return true;
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

    s_rst_back = ui_button(s_result, 16, 360, 448, 80, "返回详情", COLOR_CARD, 22);
    lv_obj_add_event_cb(s_rst_back, on_result_back, LV_EVENT_CLICKED, NULL);
}

static void show_result(const panel_action_result_t *r)
{
    switch (r->state) {
    case PANEL_ACTION_SENDING:
        lv_label_set_text(s_rst_title, "发送中…");
        lv_label_set_text(s_rst_body, "请勿重复提交");
        break;
    case PANEL_ACTION_DELIVERED:
        lv_label_set_text(s_rst_title, "已送达");
        lv_label_set_text(s_rst_body, "等待 Agent 响应");
        break;
    case PANEL_ACTION_OBSERVED:
        lv_label_set_text(s_rst_title, "已处理");
        lv_label_set_text(s_rst_body, "已观察到会话状态变化");
        break;
    case PANEL_ACTION_STALE:
        lv_label_set_text(s_rst_title, "现场已变化");
        lv_label_set_text(s_rst_body, "请求已变化，请重新查看");
        break;
    case PANEL_ACTION_UNSUPPORTED:
        lv_label_set_text(s_rst_title, "不支持此操作");
        lv_label_set_text(s_rst_body, "请在主机处理");
        break;
    case PANEL_ACTION_UNAVAILABLE:
        lv_label_set_text(s_rst_title, "鉴权或协议错误");
        lv_label_set_text(s_rst_body, "请检查网关令牌与接口版本");
        break;
    default:
        lv_label_set_text(s_rst_title, "结果未知");
        lv_label_set_text(s_rst_body, "请在主机核实；不会自动重试");
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
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    ui_label(s_settings, 80, 20, 300, "设置", 24, COLOR_TEXT);

    lv_obj_t *list = lv_obj_create(s_settings);
    lv_obj_set_pos(list, 8, 72);
    lv_obj_set_size(list, 464, 338);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    static const struct { const char *name; screen_t target; } items[] = {
        { "声音", SCR_SOUND }, { "显示", SCR_DISPLAY },
        { "会话", SCR_SESSIONS }, { "连接", SCR_CONNECTION },
        { "关于", SCR_ABOUT }, { "重新配网", SCR_REPROVISION_CONFIRM },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = ui_button(list, 8, i * 78, 448, 72, items[i].name,
                                i == 5 ? COLOR_PENDING : COLOR_CARD, 22);
        lv_obj_add_event_cb(b, on_settings_nav, LV_EVENT_CLICKED,
                            (void *)(intptr_t)items[i].target);
    }
    lv_obj_t *home = ui_button(s_settings, 16, 424, 448, 48, "返回首页", COLOR_CARD, 18);
    lv_obj_add_event_cb(home, on_back, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *make_pref_page(const char *title, screen_t parent)
{
    lv_obj_t *scr = make_screen();
    lv_obj_t *back = ui_button(scr, 8, 8, 56, 48, LV_SYMBOL_LEFT, COLOR_CARD, 22);
    lv_obj_add_event_cb(back, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)parent);
    ui_label(scr, 80, 20, 370, title, 24, COLOR_TEXT);
    if (s_pref_status_count < (int)(sizeof(s_pref_status) / sizeof(s_pref_status[0]))) {
        s_pref_status[s_pref_status_count++] =
            ui_label(scr, 16, 444, 448, "", 16, COLOR_DIM);
    }
    return scr;
}

static lv_obj_t *add_pref_button(lv_obj_t *scr, int y,
                                  const char *name, panel_pref_field_t field)
{
    lv_obj_t *b = ui_button(scr, 16, y, 448, 68, name, COLOR_CARD, 20);
    lv_obj_add_event_cb(b, on_pref_click, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(field + 1));
    if (s_pref_widget_count < (int)(sizeof(s_pref_widgets) / sizeof(s_pref_widgets[0]))) {
        s_pref_widgets[s_pref_widget_count++] = (pref_widget_t){ b, field, name };
    }
    return b;
}

static lv_obj_t *add_pref_slider(lv_obj_t *scr, int y,
                                  const char *name, panel_pref_field_t field,
                                  int min_value, int max_value)
{
    ui_label(scr, 16, y, 448, name, 18, COLOR_DIM);
    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_pos(slider, 40, y + 38);
    lv_obj_set_size(slider, 400, 20);
    lv_slider_set_range(slider, min_value, max_value);
    lv_obj_add_event_cb(slider, on_pref_slider, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)(field + 1));
    lv_obj_add_event_cb(slider, on_pref_slider, LV_EVENT_RELEASED,
                        (void *)(intptr_t)(field + 1));
    return slider;
}

static void build_pref_pages(void)
{
    s_quick = make_pref_page("快捷设置", SCR_HOME);
    add_pref_button(s_quick, 80, "声音", PANEL_PREF_SOUND_ENABLED);
    s_quick_brightness = add_pref_slider(s_quick, 174, "亮度", PANEL_PREF_BRIGHTNESS, 10, 100);
    lv_obj_t *all = ui_button(s_quick, 16, 292, 448, 80, "全部设置", COLOR_ACCENT, 22);
    lv_obj_add_event_cb(all, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_SETTINGS);

    s_sound = make_pref_page("声音", SCR_SETTINGS);
    s_sound_status = s_pref_status[s_pref_status_count - 1];
    add_pref_button(s_sound, 76, "声音", PANEL_PREF_SOUND_ENABLED);
    s_sound_volume = add_pref_slider(s_sound, 156, "音量", PANEL_PREF_SOUND_VOLUME, 0, 100);
    lv_obj_t *test_request = ui_button(s_sound, 16, 215, 214, 36,
                                       "试听请求音", COLOR_BORDER, 16);
    lv_obj_t *test_done = ui_button(s_sound, 250, 215, 214, 36,
                                    "试听完成音", COLOR_BORDER, 16);
    lv_obj_add_event_cb(test_request, on_sound_test, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PANEL_AUDIO_REQUEST);
    lv_obj_add_event_cb(test_done, on_sound_test, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PANEL_AUDIO_DONE);
    add_pref_button(s_sound, 256, "需要输入", PANEL_PREF_SOUND_REQUEST);
    add_pref_button(s_sound, 330, "任务完成", PANEL_PREF_SOUND_DONE);
    lv_obj_t *more = ui_button(s_sound, 16, 404, 448, 36, "更多声音设置", COLOR_BORDER, 16);
    lv_obj_add_event_cb(more, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_SOUND_MORE);

    s_sound_more = make_pref_page("声音选项", SCR_SOUND);
    add_pref_button(s_sound_more, 76, "提醒范围", PANEL_PREF_SOUND_SCOPE);
    add_pref_button(s_sound_more, 150, "Claude", PANEL_PREF_SOUND_CLAUDE);
    add_pref_button(s_sound_more, 224, "OpenCode", PANEL_PREF_SOUND_OPENCODE);
    add_pref_button(s_sound_more, 298, "Pi", PANEL_PREF_SOUND_PI);
    lv_obj_t *quiet = ui_button(s_sound_more, 16, 372, 448, 68, "免打扰", COLOR_CARD, 20);
    lv_obj_add_event_cb(quiet, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_QUIET);

    s_quiet = make_pref_page("免打扰", SCR_SOUND_MORE);
    add_pref_button(s_quiet, 76, "已启用", PANEL_PREF_QUIET_ENABLED);
    add_pref_button(s_quiet, 150, "开始（+15 分）", PANEL_PREF_QUIET_START);
    add_pref_button(s_quiet, 224, "结束（+15 分）", PANEL_PREF_QUIET_END);
    add_pref_button(s_quiet, 298, "时区偏移（+15 分）", PANEL_PREF_UTC_OFFSET);
    s_quiet_note = ui_label(s_quiet, 20, 382, 440, "", 16, COLOR_PENDING);

    s_display = make_pref_page("显示", SCR_SETTINGS);
    s_display_brightness = add_pref_slider(s_display, 72, "亮度", PANEL_PREF_BRIGHTNESS, 10, 100);
    add_pref_button(s_display, 166, "闲置降亮", PANEL_PREF_IDLE_DIM_SECONDS);
    add_pref_button(s_display, 236, "降亮后亮度", PANEL_PREF_DIM_BRIGHTNESS);
    add_pref_button(s_display, 306, "减少动画", PANEL_PREF_REDUCE_MOTION);
    add_pref_button(s_display, 376, "视觉提醒", PANEL_PREF_VISUAL_ALERT);

    s_sessions = make_pref_page("会话", SCR_SETTINGS);
    add_pref_button(s_sessions, 76, "刷新间隔", PANEL_PREF_OVERVIEW_INTERVAL);
    add_pref_button(s_sessions, 150, "卡片排序", PANEL_PREF_CARD_ORDER);
    add_pref_button(s_sessions, 224, "隐藏空闲", PANEL_PREF_HIDE_IDLE);
    ui_label(s_sessions, 24, 318, 432, "继续提示词：在主机配置", 18, COLOR_DIM);

    s_connection = make_pref_page("连接", SCR_SETTINGS);
    app_config_t cfg;
    app_config_get(&cfg);
    char label[180];
    snprintf(label, sizeof(label), "Wi-Fi：%s\n网关：%s:%u",
             cfg.wifi_ssid, cfg.backend_host, (unsigned)cfg.backend_port);
    lv_obj_t *info = ui_label(s_connection, 20, 100, 440, label, 20, COLOR_TEXT);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(info, 160);
    s_connection_power = ui_label(s_connection, 20, 235, 440,
                                  "电量：读取中", 18, COLOR_DIM);
    lv_obj_t *refresh = ui_button(s_connection, 16, 300, 448, 80, "刷新连接", COLOR_CARD, 20);
    lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, NULL);

    s_about = make_pref_page("关于", SCR_SETTINGS);
    ui_label(s_about, 20, 110, 440, "Herdr Panel\nESP-IDF · Panel API v1", 20, COLOR_TEXT);

    s_reprovision = make_pref_page("重新配网？", SCR_SETTINGS);
    lv_obj_t *warn = ui_label(s_reprovision, 20, 110, 440,
                              "清除 Wi-Fi 与网关令牌？\n显示和声音设置会保留。",
                              20, COLOR_TEXT);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(warn, 120);
    lv_obj_t *cancel = ui_button(s_reprovision, 16, 286, 448, 64, "取消", COLOR_CARD, 20);
    lv_obj_add_event_cb(cancel, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_SETTINGS);
    lv_obj_t *confirm = ui_button(s_reprovision, 16, 366, 448, 70,
                                  "清除连接凭据", COLOR_PENDING, 20);
    lv_obj_add_event_cb(confirm, on_menu, LV_EVENT_CLICKED, (void *)(intptr_t)3);
}

static void refresh_prefs_ui(void)
{
    panel_prefs_t p;
    panel_prefs_get(&p);
    char buf[100];
    for (int i = 0; i < s_pref_widget_count; i++) {
        pref_widget_t *w = &s_pref_widgets[i];
        int v = panel_prefs_value(&p, w->field);
        const char *value = NULL;
        if (w->field == PANEL_PREF_SOUND_SCOPE) {
            value = v == PANEL_SOUND_PAGE ? "当前页" :
                    v == PANEL_SOUND_SELECTED ? "已选会话" : "全部";
        } else if (w->field == PANEL_PREF_CARD_ORDER) {
            value = v == PANEL_ORDER_BLOCKED ? "待处理优先" :
                    v == PANEL_ORDER_RECENT ? "最近更新" : "固定顺序";
        } else if (w->field == PANEL_PREF_SOUND_CLAUDE ||
                   w->field == PANEL_PREF_SOUND_OPENCODE ||
                   w->field == PANEL_PREF_SOUND_PI) {
            value = v == PANEL_SOUND_ON ? "开" :
                    v == PANEL_SOUND_OFF ? "关" : "继承";
        } else if (w->field == PANEL_PREF_QUIET_START || w->field == PANEL_PREF_QUIET_END) {
            snprintf(buf, sizeof(buf), "%s  %02d:%02d", w->name, v / 60, v % 60);
        } else if (w->field == PANEL_PREF_UTC_OFFSET) {
            int absv = v < 0 ? -v : v;
            snprintf(buf, sizeof(buf), "%s  %c%02d:%02d", w->name,
                     v < 0 ? '-' : '+', absv / 60, absv % 60);
        } else if (w->field == PANEL_PREF_OVERVIEW_INTERVAL ||
                   w->field == PANEL_PREF_IDLE_DIM_SECONDS) {
            snprintf(buf, sizeof(buf), "%s  %d s", w->name, v);
        } else if (w->field == PANEL_PREF_DIM_BRIGHTNESS) {
            snprintf(buf, sizeof(buf), "%s  %d%%", w->name, v);
        } else {
            value = v ? "开" : "关";
        }
        if (value != NULL) snprintf(buf, sizeof(buf), "%s  %s", w->name, value);
        lv_label_set_text(lv_obj_get_child(w->button, 0), buf);
    }
    if (s_quick_brightness) lv_slider_set_value(s_quick_brightness, p.brightness, LV_ANIM_OFF);
    if (s_display_brightness) lv_slider_set_value(s_display_brightness, p.brightness, LV_ANIM_OFF);
    if (s_sound_volume) lv_slider_set_value(s_sound_volume, p.sound_volume, LV_ANIM_OFF);
    if (s_quiet_note) {
        const char *note = p.quiet_enabled && time(NULL) < 1704067200 ?
            "时间未同步，声音已静音" :
            p.quiet_enabled && p.quiet_start == p.quiet_end ?
            "起止相同：全天静音" :
            "固定时区偏移；夏令时请手动调整";
        lv_label_set_text(s_quiet_note, note);
    }
}

/* ====================================================================== */
/* PRV                                                                     */
/* ====================================================================== */

static void build_provision(void)
{
    s_prov = make_screen();
    ui_label(s_prov, 16, 24, 448, "连接设备热点", 28, COLOR_TEXT);
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
             "SSID：%s\n密码：%s\n扫码连接 Wi-Fi\n页面未打开？192.168.4.1",
             s_prov_ssid, s_prov_pass);
    lv_label_set_text(s_prov_info, info);
    if (s_screen != SCR_BOOT) show_screen(SCR_PROVISION);
}

/* ====================================================================== */
/* screen switching / events                                              */
/* ====================================================================== */

static void show_screen(screen_t s)
{
    if (s == SCR_HOME && s_screen != SCR_HOME) s_seen_generation = 0;
    screen_t previous = s_screen;
    s_screen = s;
    lv_obj_t *scr = NULL;
    switch (s) {
    case SCR_BOOT: scr = s_boot; break;
    case SCR_HOME: scr = s_home; break;
    case SCR_DETAIL: scr = s_detail; break;
    case SCR_CONFIRM: scr = s_confirm; break;
    case SCR_RESULT: scr = s_result; break;
    case SCR_SETTINGS: scr = s_settings; break;
    case SCR_QUICK: scr = s_quick; break;
    case SCR_SOUND: scr = s_sound; break;
    case SCR_SOUND_MORE: scr = s_sound_more; break;
    case SCR_QUIET: scr = s_quiet; break;
    case SCR_DISPLAY: scr = s_display; break;
    case SCR_SESSIONS: scr = s_sessions; break;
    case SCR_CONNECTION: scr = s_connection; break;
    case SCR_ABOUT: scr = s_about; break;
    case SCR_REPROVISION_CONFIRM: scr = s_reprovision; break;
    case SCR_PROVISION: scr = s_prov; break;
    }
    if (scr != NULL) {
        panel_prefs_t p;
        panel_prefs_get(&p);
        if (previous != s && !p.reduce_motion &&
            s != SCR_CONFIRM && s != SCR_PROVISION) {
            lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, false);
        } else {
            lv_screen_load(scr);
        }
        if (s == SCR_SOUND && !panel_audio_ready()) {
            lv_label_set_text(s_sound_status, "声音不可用，请检查扬声器");
        }
    }
}

static void on_card(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= 4) return;
    const char *term = s_cells[slot].terminal_id;
    if (term[0] == '\0') return;

    bump_epoch();
    snprintf(s_sel_term, sizeof(s_sel_term), "%s", term);
    panel_store_set_view_context(s_page, s_sel_term);

    panel_cmd_t cmd = {
        .type = PANEL_CMD_OPEN_DETAIL,
        .selection_epoch = s_sel_epoch,
    };
    snprintf(cmd.terminal_id, sizeof(cmd.terminal_id), "%s", term);
    if (!panel_store_enqueue_control(&cmd)) {
        /* queue full: non-blocking, show busy */
        return;
    }

    /* don't show the previous session's content while this one loads */
    lv_label_set_text(s_det_name, term);
    lv_label_set_text(s_det_status, "");
    lv_label_set_text(s_det_project, "");
    lv_label_set_text(s_det_updated, "");
    lv_label_set_text(s_det_hint, "");
    lv_obj_clean(s_det_content);
    ui_label(s_det_content, 0, 60, 424, "加载中…", 18, COLOR_DISABLED);
    lv_obj_add_flag(s_btn_allow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_deny, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_always, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_continue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host, LV_OBJ_FLAG_HIDDEN);

    show_screen(SCR_DETAIL);
}

static void on_menu(lv_event_t *e)
{
    void *ud = lv_event_get_user_data(e);
    if (ud == (void *)(intptr_t)3) {
        /* Only this confirmation page may clear credentials. */
        if (s_screen != SCR_REPROVISION_CONFIRM) return;
        if (app_config_clear_credentials() == ESP_OK) {
            esp_restart();
        } else {
            for (int i = 0; i < s_pref_status_count; i++) {
                lv_label_set_text(s_pref_status[i], "清除连接凭据失败");
            }
        }
        return;
    }
    if (s_screen == SCR_HOME) {
        refresh_prefs_ui();
        show_screen(SCR_QUICK);
    }
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (s_screen == SCR_DETAIL) {
        bump_epoch();
        s_sel_term[0] = '\0';
        panel_store_set_view_context(s_page, NULL);
        /* tell the worker to stop polling this detail */
        panel_cmd_t cmd = { .type = PANEL_CMD_CLOSE_DETAIL };
        panel_store_enqueue_control(&cmd);
        show_screen(SCR_HOME);
    } else if (s_screen == SCR_SETTINGS || s_screen == SCR_RESULT) {
        show_screen(SCR_HOME);
    } else if (s_screen == SCR_CONFIRM) {
        s_pending_action = PANEL_ACT_NONE;
        show_screen(SCR_DETAIL);
    }
}

static void on_settings_nav(lv_event_t *e)
{
    screen_t target = (screen_t)(intptr_t)lv_event_get_user_data(e);
    refresh_prefs_ui();
    show_screen(target);
}

static bool queue_pref(panel_pref_field_t field, int value)
{
    panel_cmd_t cmd = {
        .type = PANEL_CMD_SET_PREF,
        .pref_field = field,
        .pref_value = value,
        .edit_id = ++s_edit_id,
    };
    if (!panel_store_enqueue_control(&cmd)) {
        for (int i = 0; i < s_pref_status_count; i++) {
            lv_label_set_text(s_pref_status[i], "忙碌，设置未保存");
        }
        panel_prefs_t p;
        panel_prefs_get(&p);
        panel_display_set_brightness(p.brightness);
        panel_audio_set_volume(p.sound_volume);
        refresh_prefs_ui();
        return false;
    }
    for (int i = 0; i < s_pref_status_count; i++) {
        lv_label_set_text(s_pref_status[i], "保存中…");
    }
    return true;
}

static void on_pref_click(lv_event_t *e)
{
    panel_pref_field_t field = (panel_pref_field_t)((intptr_t)lv_event_get_user_data(e) - 1);
    panel_prefs_t p;
    panel_prefs_get(&p);
    int v = panel_prefs_value(&p, field);
    int next = v;
    switch (field) {
    case PANEL_PREF_SOUND_ENABLED: case PANEL_PREF_SOUND_REQUEST:
    case PANEL_PREF_SOUND_DONE: case PANEL_PREF_QUIET_ENABLED:
    case PANEL_PREF_REDUCE_MOTION: case PANEL_PREF_VISUAL_ALERT:
    case PANEL_PREF_HIDE_IDLE: next = !v; break;
    case PANEL_PREF_SOUND_SCOPE: case PANEL_PREF_SOUND_CLAUDE:
    case PANEL_PREF_SOUND_OPENCODE: case PANEL_PREF_SOUND_PI:
    case PANEL_PREF_CARD_ORDER: next = (v + 1) % 3; break;
    case PANEL_PREF_QUIET_START: case PANEL_PREF_QUIET_END:
        next = (v + 15) % 1440; break;
    case PANEL_PREF_UTC_OFFSET: next = v >= 840 ? -720 : v + 15; break;
    case PANEL_PREF_IDLE_DIM_SECONDS:
        next = v == 0 ? 30 : v == 30 ? 60 :
               v == 60 ? 120 : v == 120 ? 300 : 0;
        break;
    case PANEL_PREF_DIM_BRIGHTNESS: next = v >= 50 ? 5 : v + 5; break;
    case PANEL_PREF_OVERVIEW_INTERVAL:
        next = v == 2 ? 3 : v == 3 ? 5 : v == 5 ? 10 : 2;
        break;
    default: return;
    }
    if (field == PANEL_PREF_SOUND_ENABLED && next == 0) panel_audio_stop();
    queue_pref(field, next);
}

static void on_pref_slider(lv_event_t *e)
{
    panel_pref_field_t field = (panel_pref_field_t)((intptr_t)lv_event_get_user_data(e) - 1);
    lv_obj_t *slider = lv_event_get_target(e);
    int value = lv_slider_get_value(slider);
    int step = field == PANEL_PREF_BRIGHTNESS ? 5 : 10;
    value = ((value + step / 2) / step) * step;
    if (field == PANEL_PREF_BRIGHTNESS) {
        if (value < 10) value = 10;
        panel_display_set_brightness((uint8_t)value);
    } else if (field == PANEL_PREF_SOUND_VOLUME) {
        static int64_t last_preview_ms;
        int64_t now_ms = ui_now_ms();
        if (lv_event_get_code(e) == LV_EVENT_RELEASED ||
            now_ms - last_preview_ms >= 150) {
            panel_audio_set_volume((uint8_t)value);
            last_preview_ms = now_ms;
        }
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) queue_pref(field, value);
}

static void on_sound_test(lv_event_t *e)
{
    panel_prefs_t prefs;
    panel_prefs_get(&prefs);
    if (!panel_audio_ready()) {
        lv_label_set_text(s_sound_status, "声音不可用，请检查扬声器");
    } else if (lv_slider_get_value(s_sound_volume) == 0) {
        lv_label_set_text(s_sound_status, "请先调高音量");
    } else {
        bool queued = panel_audio_play((panel_audio_kind_t)(intptr_t)lv_event_get_user_data(e));
        lv_label_set_text(s_sound_status, queued ?
                          "仅试听；静音设置未改变" : "声音忙碌，请重试");
    }
}

static void on_refresh(lv_event_t *e)
{
    (void)e;
    panel_cmd_t cmd = { .type = PANEL_CMD_REFRESH };
    panel_store_enqueue_control(&cmd);
}

static void on_jump_blocked(lv_event_t *e)
{
    (void)e;
    /* spec §2.2: tapping the blocked count jumps to the first blocked card */
    int idx = panel_store_first_blocked_index();
    if (idx >= 0) {
        s_page = idx / PAGE_SIZE;
        panel_store_set_view_context(s_page, s_sel_term);
        s_seen_generation = 0;  /* force redraw on next tick */
    }
}

static void on_home_gesture(lv_event_t *e)
{
    (void)e;
    if (s_screen != SCR_HOME) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) page_next();
    else if (dir == LV_DIR_RIGHT) page_prev();
}

static void on_action_btn(lv_event_t *e)
{
    panel_action_id_t act = (panel_action_id_t)(intptr_t)lv_event_get_user_data(e);
    panel_detail_t det;
    if (!panel_store_get_detail(&det)) return;
    /* the stored detail must still be the session the user is looking at;
     * the worker already discards responses with a stale selection epoch */
    if (strcmp(det.terminal_id, s_sel_term) != 0 ||
        det.selection_epoch != s_sel_epoch || det.gone ||
        panel_store_conn_state() != PANEL_CONN_ONLINE ||
        det.fetched_at_ms <= 0 ||
        ui_now_ms() - det.fetched_at_ms > 10000 ||
        !(det.pending.choices & action_choice(act))) return;
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
    if (!confirmed_request_current()) {
        s_pending_action = PANEL_ACT_NONE;
        show_screen(SCR_DETAIL);
        lv_label_set_text(s_det_hint, "请求已变化或过期，请刷新后重看");
        return;
    }
    if (panel_store_action_reserved()) {
        show_result(&(panel_action_result_t){
            .state = PANEL_ACTION_SENDING,
        });
        return;
    }
    if (!panel_store_try_reserve_action()) {
        lv_label_set_text(s_cnf_hint, "忙碌，请稍后重试");
        return;
    }

    panel_cmd_t cmd = {
        .type = PANEL_CMD_ACTION,
        .action = s_pending_action,
        .selection_epoch = s_sel_epoch,
    };
    snprintf(cmd.terminal_id, sizeof(cmd.terminal_id), "%s", s_sel_term);
    snprintf(cmd.context_token, sizeof(cmd.context_token), "%s", s_frozen_pending.context_token);
    if (s_pending_action == PANEL_ACT_CONTINUE &&
        s_frozen_pending.source == PANEL_SOURCE_NEW_PROMPT) {
        snprintf(cmd.prompt, sizeof(cmd.prompt), "%s", s_frozen_pending.prompt);
    }

    if (!panel_store_enqueue_action(&cmd)) {
        panel_store_release_action();
        lv_label_set_text(s_cnf_hint, "忙碌，请稍后重试");
        return;
    }

    s_pending_action = PANEL_ACT_NONE;
    panel_action_result_t sending = { .state = PANEL_ACTION_SENDING };
    snprintf(sending.message, sizeof(sending.message), "发送中…");
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
    build_agent_icons();
    build_boot();
    build_home();
    build_detail();
    build_confirm();
    build_result();
    build_settings();
    build_pref_pages();
    build_provision();
    panel_store_set_view_context(s_page, NULL);
    refresh_prefs_ui();
    refresh_power_ui();
    s_boot_started_ms = ui_now_ms();
    show_screen(SCR_BOOT);
}

void ui_show_provisioning(const char *ap_ssid, const char *ap_pass, const char *qr_payload)
{
    s_boot_provision = true;
    show_provision_screen(ap_ssid, ap_pass, qr_payload);
}

void ui_panel_boot_ready(void)
{
    s_boot_ready = true;
}

void ui_panel_tick(void)
{
    refresh_power_ui();
    if (s_screen == SCR_BOOT) tick_boot();
    /* 1. drain one-shot UI events (action results etc.) */
    panel_ui_evt_t evt;
    while (panel_store_recv_ui_event(&evt, 0)) {
        if (evt.type == PANEL_EVT_ACTION_RESULT) {
            if (s_screen == SCR_CONFIRM || s_screen == SCR_RESULT) {
                show_result(&evt.action);
            }
        } else if (evt.type == PANEL_EVT_CONFIG_RESULT) {
            panel_prefs_t p;
            panel_prefs_get(&p);
            if (evt.pref_field == PANEL_PREF_BRIGHTNESS) {
                panel_display_set_brightness(p.brightness);
            }
            refresh_prefs_ui();
            s_seen_generation = 0;
            for (int i = 0; i < s_pref_status_count; i++) {
                lv_label_set_text(s_pref_status[i], evt.message);
            }
        }
    }

    if (s_screen == SCR_CONFIRM && !confirmed_request_current()) {
        s_pending_action = PANEL_ACT_NONE;
        show_screen(SCR_DETAIL);
        lv_label_set_text(s_det_hint, "请求已变化或过期，请刷新后重看");
    }

    panel_prefs_t display_prefs;
    panel_prefs_get(&display_prefs);
    bool should_dim = display_prefs.idle_dim_seconds != 0 &&
        s_screen != SCR_CONFIRM && s_screen != SCR_PROVISION &&
        lv_display_get_inactive_time(NULL) >=
            (uint32_t)display_prefs.idle_dim_seconds * 1000u;
    if (should_dim != s_dimmed) {
        s_dimmed = should_dim;
        panel_display_set_brightness(should_dim ?
                                     display_prefs.dim_brightness :
                                     display_prefs.brightness);
    }

    /* 2. redraw when store generation changed */
    uint32_t gen = panel_store_generation();
    bool gen_changed = (gen != s_seen_generation);
    s_seen_generation = gen;

    if (s_screen == SCR_HOME) {
        update_alert_pulses();
        if (!gen_changed) return;

        panel_agent_card_t cards[4];
        int count = 0, total = 0;
        panel_conn_state_t conn;
        panel_store_get_page(cards, s_page, &count, &total, &conn);
        s_card_count = count;

        /* clamp the page when the list shrank, then refetch that page */
        int pages = (count + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages < 1) pages = 1;
        if (s_page >= pages) {
            s_page = pages - 1;
            panel_store_get_page(cards, s_page, &count, &total, &conn);
            panel_store_set_view_context(s_page, s_sel_term);
        }
        if (s_page < 0) s_page = 0;

        refresh_home(cards, count, total, conn);
        return;
    }

    if (s_screen != SCR_DETAIL) return;

    /* detail: refresh age display every second and re-render when either
     * the store changed or the freshness category flipped (spec §4.3) */
    panel_detail_t det;
    if (!panel_store_get_detail(&det)) return;
    if (strcmp(det.terminal_id, s_sel_term) != 0) return;

    panel_conn_state_t conn = panel_store_conn_state();

    int age_s = det.fetched_at_ms > 0
                    ? (int)((ui_now_ms() - det.fetched_at_ms) / 1000)
                    : 0;
    if (age_s < 0) age_s = 0;

    bool fresh = conn == PANEL_CONN_ONLINE && age_s <= 10;
    bool stale_flag = age_s > 6;
    bool category_flip = fresh != s_prev_fresh || stale_flag != (s_last_age_sec > 6);

    if (gen_changed || category_flip) {
        s_prev_fresh = fresh;
        s_last_age_sec = age_s;
        render_detail(&det, conn, age_s);
    } else if (age_s != s_last_age_sec) {
        /* cheap path: update only the age label */
        s_last_age_sec = age_s;
        char ubuf[48];
        if (stale_flag) {
            snprintf(ubuf, sizeof(ubuf), "数据已过期 %d 秒", age_s);
        } else {
            snprintf(ubuf, sizeof(ubuf), "更新于 %d 秒前", age_s);
        }
        lv_label_set_text(s_det_updated, ubuf);
    }
}
