/*
 * LVGL UI — Herdr decision panel (UI_DESIGN.md v1).
 *
 * HOM  2-column scrolling grid of sessions
 * DET  session detail + 3 icon action slots (gateway choices only)
 * CNF  full-screen confirm before any semantic action
 * RST  sending / delivered / uncertain result
 * SET  connection, display, about, re-provision (view + simple toggles)
 * PRV  WPA2 provisioning QR
 * STB  idle standby: breathing clock, then fully dark (UI_DESIGN.md §4.3)
 *
 * Selection is (terminal_id, selection_epoch). Late network responses with
 * a mismatched epoch are discarded in panel_worker before reaching here.
 */

#include "ui_panel.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "ui_panel";

#include "app_config.h"
#include "panel_model.h"
#include "panel_store.h"
#include "panel_prefs.h"
#include "panel_display.h"
#include "panel_audio.h"
#include "panel_power.h"
#include "panel_motion.h"
#include "provisioning.h"
#include "ui_common.h"

#define SCREEN_W     480
#define SCREEN_H     480

/* UI_DESIGN.md §4.1 home is a compact two-column scrolling grid, so a wall of
 * sessions stays scannable instead of costing one screen per few rows. The
 * grid scrolls vertically (LVGL native), so there is no paging, no swipe
 * handler and no page counter any more. */
#define LIST_TOP        70      /* below the top bar */
#define LIST_BOTTOM     402     /* above the notice row */
#define LIST_X          8
#define LIST_W          (SCREEN_W - 2 * LIST_X)
#define CARD_H          78
#define CARD_W          ((LIST_W - CARD_GAP) / 2)
#define CARD_GAP        6
/* Reusable card widgets. 10 rows = 20 sessions, five screenfuls; beyond that
 * the store caps the overview at PANEL_MAX_AGENTS. */
#define VISIBLE_ROWS    10

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
    SCR_STANDBY,
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
/* Standby (UI_DESIGN.md §4.3): a big clock over three breathing rings. Built
 * lazily on the first idle timeout so it costs nothing on the normal path. */
static lv_obj_t *s_standby, *s_standby_clock, *s_standby_date, *s_standby_note;
static lv_obj_t *s_standby_ring[3];
static lv_obj_t *s_quick, *s_sound, *s_sound_more, *s_quiet;
static lv_obj_t *s_display, *s_sessions, *s_connection, *s_about, *s_reprovision;

static lv_obj_t *s_conn_badge, *s_pending_count;
static lv_obj_t *s_battery_label, *s_battery_fill, *s_connection_power;
static lv_obj_t *s_sound_notice;
static lv_obj_t *s_empty_label;
/* Home is one vertical scrolling list (UI_DESIGN.md §4.1). The cell array is
 * a pool of reusable rows: s_cell_count rows exist, and a card is either
 * bound to a session (terminal_id non-empty) or shown as an empty slot. */
static lv_obj_t *s_card_list;
static cell_t s_cells[PANEL_MAX_AGENTS];
static int s_cell_count;

static lv_obj_t *s_det_name, *s_det_status, *s_det_project, *s_det_updated;
static lv_obj_t *s_det_content;
/* Three action slots in the upper half (UI_DESIGN.md §4.2). The old 88 px
 * bottom button row was removed so the content area could grow. */
static lv_obj_t *s_act_slot[3], *s_act_glyph[3];
static lv_obj_t *s_det_act_caption;
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

static lv_obj_t *s_prov_qr, *s_prov_info, *s_prov_cancel;

/* ---- local view state -------------------------------------------------- */

static int s_card_count;
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

/* Standby / blank state machine (UI_DESIGN.md §4.3). */
typedef enum {
    SB_AWAKE = 0,      /* normal UI */
    SB_CLOCK,          /* idle long enough: clock screen, dimmed */
    SB_BLANK,          /* idle longer still: screen fully dark */
} standby_stage_t;
static standby_stage_t s_sb_stage;
static int s_sb_last_minute = -1;   /* forces a redraw when the minute flips */
static int s_sb_clock_built;

static void log_ui_memory(const char *phase)
{
    lv_mem_monitor_t mon = { 0 };
    lv_mem_monitor(&mon);
    ESP_LOGI("ui_panel", "%s: LVGL heap free=%u largest=%u used=%u%%",
             phase, (unsigned)mon.free_size,
             (unsigned)mon.free_biggest_size, (unsigned)mon.used_pct);
}

static uint32_t s_edit_id;

/*
 * One-shot font self-test (UI_DESIGN.md §2). CJK text rendered blank while
 * Latin rendered fine, which is either a glyph lookup miss or a layout
 * problem. Ask LVGL directly instead of guessing: report whether the glyph
 * descriptor is found, its advance/box size and the resolved font.
 */
static void log_font_probe(void)
{
    /* lv_font_get_glyph_dsc() takes a Unicode code point, not a UTF-8 byte. */
    static const struct { const char *label; uint32_t cp; } probe[] = {
        { "latin-A",  0x0041 },   /* A */
        { "hui",      0x4F1A },
        { "hua",      0x8BDD },
        { "wu",       0x65E0 },
        { "xian",     0x7EBF },
    };
    lv_font_t *f = ui_font(20);
    ESP_LOGI(TAG, "probe font=%p line_height=%d base_line=%d fallback=%p",
             (void *)f, (int)f->line_height, (int)f->base_line, (void *)f->fallback);
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        lv_font_glyph_dsc_t dsc;
        memset(&dsc, 0, sizeof(dsc));
        bool found = lv_font_get_glyph_dsc(f, &dsc, probe[i].cp, 0);
        ESP_LOGI(TAG, "%s U+%04X found=%d adv_w=%d box_w=%d box_h=%d "
                 "ofs_x=%d ofs_y=%d stride=%d resolved=%p",
                 probe[i].label, (unsigned)probe[i].cp,
                 (int)found, (int)dsc.adv_w, (int)dsc.box_w, (int)dsc.box_h,
                 (int)dsc.ofs_x, (int)dsc.ofs_y, (int)dsc.stride,
                 (void *)dsc.resolved_font);
    }
}

/* provisioning copy */
static char s_prov_ssid[24];
static char s_prov_pass[12];
static char s_prov_qr_text[128];
static int s_prov_remaining_last = -2;

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
static void refresh_prefs_ui(void);
static void on_pref_click(lv_event_t *e);
static void on_pref_slider(lv_event_t *e);
static void on_settings_nav(lv_event_t *e);
static void ensure_settings_pages(screen_t target);
static void on_sound_test(lv_event_t *e);
static void on_connection_edit(lv_event_t *e);
static void on_cancel_edit(lv_event_t *e);

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

/* Same stage ladder panel_prefs.c validates against; kept in one place here
 * so the settings button and its label cannot drift apart. */
static const uint16_t k_sb_stages[] = {
    PANEL_STANDBY_OFF, PANEL_STANDBY_1MIN, PANEL_STANDBY_2MIN,
    PANEL_STANDBY_3MIN, PANEL_STANDBY_5MIN, PANEL_STANDBY_10MIN,
    PANEL_STANDBY_15MIN,
};
#define SB_STAGE_N (sizeof(k_sb_stages) / sizeof(k_sb_stages[0]))

static int standby_next(int v)
{
    for (size_t i = 0; i < SB_STAGE_N; i++) {
        if (k_sb_stages[i] == v) {
            return k_sb_stages[(i + 1) % SB_STAGE_N];
        }
    }
    return PANEL_STANDBY_OFF;
}

/* Settings pages are English by convention (UI_DESIGN.md §2.1). */
static const char *standby_duration(int seconds)
{
    if (seconds < 60) return "1 min";
    if (seconds % 60 == 0) {
        switch (seconds / 60) {
        case 1:  return "1 min";
        case 2:  return "2 min";
        case 3:  return "3 min";
        case 5:  return "5 min";
        case 10: return "10 min";
        case 15: return "15 min";
        default: return "Off";
        }
    }
    return "Off";
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
/* STB — standby clock (UI_DESIGN.md §4.3)                               */
/* ====================================================================== */

/*
 * A big HH:MM over three concentric rings that breathe out of phase. The
 * motion matters more than the digits: an AMOLED left on a static image
 * burns the pixels in, so the rings change shape continuously and the clock
 * area is the only still part. Everything is built on entry to standby and
 * torn down on exit, so the normal path pays nothing for it.
 */
static void build_standby(void)
{
    if (s_sb_clock_built) return;
    s_sb_clock_built = 1;

    s_standby = make_screen();

    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(s_standby);
        /* 40 px apart in radius, so the three rings read as one pulse */
        int d = 300 + i * 40;
        lv_obj_set_size(ring, d, d);
        lv_obj_center(ring);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_20, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        s_standby_ring[i] = ring;
    }

    s_standby_clock = ui_label(s_standby, 40, 190, 400, "--:--", 24, COLOR_TEXT);
    lv_obj_set_style_text_align(s_standby_clock, LV_TEXT_ALIGN_CENTER, 0);
    /* Scale the glyphs up: ui_font(24) is the largest cut we carry and a
     * clock is the one place where size is worth the pixels. LVGL 9 scales
     * around the pivot, so centre the pivot on the label first. */
    lv_obj_set_style_transform_pivot_x(s_standby_clock, 200, 0);
    lv_obj_set_style_transform_pivot_y(s_standby_clock, 16, 0);
    lv_obj_set_style_transform_scale(s_standby_clock, 256, 0);

    s_standby_date = ui_label(s_standby, 40, 292, 400, "", 16, COLOR_DIM);
    lv_obj_set_style_text_align(s_standby_date, LV_TEXT_ALIGN_CENTER, 0);

    s_standby_note = ui_label(s_standby, 40, 414, 400,
                              LV_SYMBOL_UP " 触摸或摇一摇唤醒", 14, COLOR_DISABLED);
    lv_obj_set_style_text_align(s_standby_note, LV_TEXT_ALIGN_CENTER, 0);
}

/* Local wall clock, honouring the same fixed UTC offset the rest of the
 * firmware uses for quiet hours (panel_worker.c quiet_now()). SNTP keeps
 * time() in UTC, so shifting here keeps one source of truth. */
static void standby_localtime(struct tm *out)
{
    time_t now = time(NULL);
    panel_prefs_t p;
    panel_prefs_get(&p);
    if (now < 1704067200) {          /* clock never synced */
        memset(out, 0, sizeof(*out));
        return;
    }
    now += p.utc_offset_minutes * 60;
    gmtime_r(&now, out);
}

static const char *WEEKDAY_NAMES[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};
static const char *MONTH_NAMES[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static void standby_refresh_clock(void)
{
    struct tm local;
    standby_localtime(&local);
    char buf[64];
    if (local.tm_year == 70) {       /* unsynced sentinel from standby_localtime */
        lv_label_set_text(s_standby_clock, "--:--");
        lv_label_set_text(s_standby_date, "等待网络时间");
        s_sb_last_minute = -1;
        return;
    }
    snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    lv_label_set_text(s_standby_clock, buf);
    snprintf(buf, sizeof(buf), "%s %s %d", WEEKDAY_NAMES[local.tm_wday],
             MONTH_NAMES[local.tm_mon], local.tm_year + 1900);
    lv_label_set_text(s_standby_date, buf);
    s_sb_last_minute = local.tm_hour * 60 + local.tm_min;
}

/* Ring 0 leads, rings 1 and 2 trail by a third of a cycle each, so the
 * pattern never looks like a single blinking dot. 6 s per breath. */
static void standby_animate(int64_t now_ms)
{
    for (int i = 0; i < 3; i++) {
        int64_t phase = now_ms + (int64_t)i * 2000;
        int64_t t = phase % 6000;
        /* triangle 0..4096..0 gives a symmetric rise and fall */
        int32_t tri = t < 3000 ? (int32_t)(t * 4096 / 3000)
                               : (int32_t)((6000 - t) * 4096 / 3000);
        /* scale 80%..100% and opacity 10%..45% */
        int32_t scale = 2048 + tri / 2;             /* 256 == 100% */
        lv_obj_set_style_transform_scale(s_standby_ring[i], scale, 0);
        lv_obj_set_style_transform_pivot_x(s_standby_ring[i],
                                           lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(s_standby_ring[i],
                                           lv_pct(50), 0);
        lv_obj_set_style_border_opa(s_standby_ring[i],
                                    (lv_opa_t)(LV_OPA_10 + tri / 100), 0);
    }
}

static void enter_standby(standby_stage_t stage)
{
    build_standby();
    panel_prefs_t p;
    panel_prefs_get(&p);

    standby_refresh_clock();
    standby_animate(ui_now_ms());

    s_screen = SCR_STANDBY;
    /* No transition: a fade in from the session list would flash the whole
     * panel at a moment the user is not looking. */
    lv_screen_load(s_standby);
    panel_display_set_brightness(p.dim_brightness);
    s_dimmed = true;
    s_sb_stage = stage;
    ESP_LOGI(TAG, "standby: clock after %d s idle", p.idle_display_seconds);
}

static void leave_standby(void)
{
    if (s_screen != SCR_STANDBY) return;
    s_sb_stage = SB_AWAKE;
    s_sb_last_minute = -1;
    /* The real screen was never torn down, so hand control back to it and
     * force a refresh; the store generation may be many polls old. */
    s_seen_generation = 0;
    show_screen(SCR_HOME);
    panel_prefs_t p;
    panel_prefs_get(&p);
    panel_display_set_brightness(p.brightness);
    s_dimmed = false;
    ESP_LOGI(TAG, "standby cleared");
}

/*
 * The one place that decides which standby stage applies (UI_DESIGN.md §4.3).
 * Called from the LVGL tick.
 *
 * Touch needs no code: any indev activity rewrites disp->last_activity_time,
 * which is exactly what lv_display_get_inactive_time() reports, so a tap
 * drops the elapsed time back to zero by itself. Motion has no such side
 * effect, so panel_motion_wake_pending() is consumed here.
 *
 * Confirm and provisioning are excluded entirely. Both are modal and
 * short-lived: blanking the confirm page would hide the very text the user
 * is being asked to read, and the provisioning QR has to stay up until a
 * phone has scanned it.
 */
static void update_standby(void)
{
    panel_prefs_t p;
    panel_prefs_get(&p);

    if (s_screen == SCR_STANDBY) {
        bool woke = (p.motion_wake && panel_motion_wake_pending());
        uint32_t idle_ms = lv_display_get_inactive_time(NULL);
        if (woke || idle_ms < 500) {
            leave_standby();
            return;
        }
        if (p.idle_blank_seconds != 0 &&
            idle_ms >= (uint32_t)p.idle_blank_seconds * 1000u) {
            /* Fully dark: an AMOLED with no pixels lit draws almost no power
             * and cannot burn in. The widgets stay alive behind the black
             * screen, so waking costs nothing and no rebuild is needed. */
            if (s_sb_stage != SB_BLANK) {
                panel_display_set_brightness(0);
                s_sb_stage = SB_BLANK;
                ESP_LOGI(TAG, "standby: blank after %d s idle",
                         p.idle_blank_seconds);
            }
            return;
        }
        standby_animate(ui_now_ms());
        struct tm local;
        standby_localtime(&local);
        if (local.tm_year != 70 &&
            local.tm_hour * 60 + local.tm_min != s_sb_last_minute) {
            standby_refresh_clock();
        }
        return;
    }

    if (s_screen == SCR_CONFIRM || s_screen == SCR_PROVISION) return;
    if (p.idle_display_seconds == 0) return;
    if (lv_display_get_inactive_time(NULL) <
        (uint32_t)p.idle_display_seconds * 1000u) return;

    enter_standby(SB_CLOCK);
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

    /*
     * Two-column wrapping grid inside one vertically scrolling container.
     * Cards stay clickable and scrolling is the container's native
     * behaviour, so a swipe can no longer be mistaken for a card tap: once
     * the container scrolls, LVGL suppresses CLICKED on the card underneath.
     */
    s_card_list = lv_obj_create(s_home);
    lv_obj_set_pos(s_card_list, LIST_X - 4, LIST_TOP - 4);
    lv_obj_set_size(s_card_list, LIST_W + 8, LIST_BOTTOM - LIST_TOP + 8);
    lv_obj_set_style_bg_opa(s_card_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_card_list, 0, 0);
    lv_obj_set_style_radius(s_card_list, 0, 0);
    lv_obj_set_style_pad_all(s_card_list, 0, 0);
    lv_obj_set_style_pad_row(s_card_list, CARD_GAP, 0);
    lv_obj_set_style_pad_column(s_card_list, CARD_GAP, 0);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);

    /* reusable card rows; content is filled/cleared by refresh_home() */
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        cell_t *c = &s_cells[i];
        c->card = lv_obj_create(s_card_list);
        lv_obj_set_size(c->card, CARD_W, CARD_H);
        lv_obj_set_style_bg_color(c->card, lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c->card, 10, 0);
        lv_obj_set_style_pad_all(c->card, 0, 0);
        lv_obj_clear_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c->card, on_card, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        /* compact cell: state badge + name on top, agent/pane underneath */
        c->shape = lv_obj_create(c->card);
        lv_obj_set_pos(c->shape, 8, 7);
        lv_obj_set_size(c->shape, 18, 18);
        lv_obj_set_style_radius(c->shape, 9, 0);
        lv_obj_set_style_bg_opa(c->shape, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c->shape, 0, 0);
        lv_obj_clear_flag(c->shape, LV_OBJ_FLAG_SCROLLABLE);

        c->shape_lbl = lv_label_create(c->shape);
        lv_label_set_text(c->shape_lbl, "");
        lv_obj_set_style_text_font(c->shape_lbl, ui_font(14), 0);
        lv_obj_set_style_text_color(c->shape_lbl, lv_color_hex(COLOR_BG), 0);
        lv_obj_center(c->shape_lbl);

        c->name = ui_label(c->card, 32, 6, CARD_W - 40, "---", 16, COLOR_TEXT);
        c->status = ui_label(c->card, 32, 26, CARD_W - 40, "", 14, COLOR_UNKNOWN);

        c->icon = lv_image_create(c->card);
        lv_obj_set_pos(c->icon, 9, 50);
        lv_image_set_scale(c->icon, 512);   /* 2x nearest: keeps the 8-bit look */

        c->agent = ui_label(c->card, 32, 50, CARD_W - 40, "", 12, COLOR_DIM);
        c->pane = ui_label(c->card, 8, CARD_H - 20, CARD_W - 16, "", 12,
                           COLOR_DISABLED);
        lv_obj_set_style_text_align(c->pane, LV_TEXT_ALIGN_RIGHT, 0);
        c->terminal_id[0] = '\0';
        c->last_status = PANEL_AGENT_UNKNOWN;
        c->alert_start_ms = 0;
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_HIDDEN);
    }
    s_cell_count = VISIBLE_ROWS;

    /* empty state (0 sessions) */
    s_empty_label = ui_label(s_home, 40, 200, 400, "暂无活跃会话", 24, COLOR_DIM);
    lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    /* bottom row: transient notice + session total (no page counter) */
    s_sound_notice = ui_label(s_home, 16, 406, 448, "", 14, COLOR_PENDING);
    lv_obj_t *hint = ui_label(s_home, 16, 438, 448, LV_SYMBOL_UP " scroll " LV_SYMBOL_DOWN,
                              16, COLOR_DISABLED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
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
    for (int i = 0; i < s_cell_count; i++) {
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

static void refresh_home(const panel_agent_card_t *cards, int count,
                         int total, panel_conn_state_t conn)
{
    lv_label_set_text(s_conn_badge, panel_conn_state_name(conn));
    lv_obj_set_style_text_color(s_conn_badge,
        lv_color_hex(conn == PANEL_CONN_ONLINE ? COLOR_IDLE : COLOR_PENDING), 0);

    char buf[48];
    snprintf(buf, sizeof(buf), "待处理 %d", panel_store_blocked_count());
    lv_label_set_text(s_pending_count, buf);

    /* No page counter any more: the list scrolls. The total still matters
     * because the store caps the overview, so say how many are listed. */
    snprintf(buf, sizeof(buf), "共 %d 个会话", total);
    lv_label_set_text(s_sound_notice, buf);
    char notice[PANEL_MESSAGE_LEN];
    panel_store_get_sound_notice(notice, sizeof(notice));
    if (notice[0] != '\0') lv_label_set_text(s_sound_notice, notice);

    if (count == 0) {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < s_cell_count; i++) {
            lv_obj_add_flag(s_cells[i].card, LV_OBJ_FLAG_HIDDEN);
            s_cells[i].terminal_id[0] = '\0';
            s_cells[i].last_status = PANEL_AGENT_UNKNOWN;
            s_cells[i].alert_start_ms = 0;
        }
        return;
    }
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    /* Bind the first `count` rows. Rows beyond the pool size are not shown;
     * the store already caps the overview at PANEL_MAX_AGENTS. */
    int rows = count < s_cell_count ? count : s_cell_count;
    for (int i = 0; i < s_cell_count; i++) {
        if (i < rows) {
            lv_obj_clear_flag(s_cells[i].card, LV_OBJ_FLAG_HIDDEN);
            fill_cell(i, &cards[i]);
        } else {
            lv_obj_add_flag(s_cells[i].card, LV_OBJ_FLAG_HIDDEN);
            s_cells[i].terminal_id[0] = '\0';
            s_cells[i].last_status = PANEL_AGENT_UNKNOWN;
            s_cells[i].alert_start_ms = 0;
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

    /* Action icons (UI_DESIGN.md §4.2): three fixed slots in the upper half.
     * A slot lights up only when the gateway offered that choice in
     * `choices`; unavailable slots stay dimmed and inert. The board has no
     * application buttons (every GPIO is taken by LCD/I2C/touch/I2S), so
     * tapping a slot is the only way to act. */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *slot = lv_button_create(s_detail);
        lv_obj_set_pos(slot, 148 + i * 64, 108);
        lv_obj_set_size(slot, 56, 56);
        lv_obj_set_style_bg_color(slot, lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(slot, 28, 0);
        lv_obj_set_style_border_width(slot, 2, 0);
        lv_obj_set_style_border_color(slot, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(slot, on_action_btn, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *glyph = lv_label_create(slot);
        lv_label_set_text(glyph, "");
        lv_obj_set_style_text_font(glyph, ui_font(24), 0);
        lv_obj_set_style_text_color(glyph, lv_color_hex(COLOR_DISABLED), 0);
        lv_obj_center(glyph);
        s_act_slot[i] = slot;
        s_act_glyph[i] = glyph;
    }
    lv_obj_t *act_cap = ui_label(s_detail, 16, 168, 448, "", 14, COLOR_DISABLED);
    lv_obj_set_style_text_align(act_cap, LV_TEXT_ALIGN_CENTER, 0);
    s_det_act_caption = act_cap;

    s_det_content = lv_obj_create(s_detail);
    lv_obj_set_pos(s_det_content, 16, 192);
    lv_obj_set_size(s_det_content, 448, 232);
    lv_obj_set_style_bg_color(s_det_content, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_det_content, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_det_content, 12, 0);
    lv_obj_set_style_border_color(s_det_content, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_det_content, 1, 0);
    lv_obj_set_style_pad_all(s_det_content, 12, 0);

    s_det_hint = ui_label(s_detail, 16, 434, 448, "操作前请确认", 16, COLOR_DIM);
    lv_obj_set_style_text_align(s_det_hint, LV_TEXT_ALIGN_CENTER, 0);
}

/*
 * Layout self-test for the CJK-blank symptom. Glyph lookup is proven healthy
 * by log_font_probe(), so this reports the geometry instead: if a label's
 * height collapses or the text is clipped away, that is why the Chinese line
 * renders blank while Latin (narrower, fewer wrapped rows) fits.
 */
static void log_detail_layout(lv_obj_t *parent, const char *what)
{
    uint32_t n = lv_obj_get_child_count(parent);
    ESP_LOGI(TAG, "layout[%s] children=%u parent=%dx%d", what, (unsigned)n,
             (int)lv_obj_get_width(parent), (int)lv_obj_get_height(parent));
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(parent, (int32_t)i);
        if (c == NULL) continue;
        ESP_LOGI(TAG, "  child%u %dx%d at (%d,%d)",
                 (unsigned)i, (int)lv_obj_get_width(c), (int)lv_obj_get_height(c),
                 (int)lv_obj_get_x(c), (int)lv_obj_get_y(c));
    }
}

/*
 * Action slots (UI_DESIGN.md §4.2). Slot meaning depends on whether the
 * gateway is asking for a decision:
 *   approval/question           -> [allow once] [deny] [allow all]
 *   idle/done, nothing to decide -> [continue] [stop] [auto]
 * A slot lights up only when the gateway actually offered that choice, so the
 * firmware never offers a request the gateway would reject. STOP is absent
 * from the gateway's Action literal (refer/herdr-restful/backend/app/panel/
 * view_models.py) and therefore stays dimmed with a "press Esc on the host"
 * caption until the gateway grows support for it.
 */
static panel_action_id_t s_act_action[3];

static void set_act_slot(int i, const char *glyph, uint32_t color, bool on)
{
    lv_label_set_text(s_act_glyph[i], glyph);
    if (on) {
        lv_obj_set_style_bg_color(s_act_slot[i], lv_color_hex(color), 0);
        lv_obj_set_style_text_color(s_act_glyph[i], lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_border_color(s_act_slot[i], lv_color_hex(color), 0);
        lv_obj_remove_state(s_act_slot[i], LV_STATE_DISABLED);
    } else {
        lv_obj_set_style_bg_color(s_act_slot[i], lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_border_color(s_act_slot[i], lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_text_color(s_act_glyph[i], lv_color_hex(COLOR_DISABLED), 0);
        lv_obj_add_state(s_act_slot[i], LV_STATE_DISABLED);
    }
}

static void dim_act_slots(void)
{
    static const char *const glyphs[3] = {
        LV_SYMBOL_OK, LV_SYMBOL_CLOSE, LV_SYMBOL_SETTINGS
    };
    for (int i = 0; i < 3; i++) {
        set_act_slot(i, glyphs[i], COLOR_DISABLED, false);
        s_act_action[i] = PANEL_ACT_NONE;
    }
    lv_label_set_text(s_det_act_caption, "");
}

static void render_action_slots(const panel_detail_t *det, uint8_t choices,
                                bool online)
{
    bool deciding = det->pending.kind == PANEL_PENDING_APPROVAL ||
                    det->pending.kind == PANEL_PENDING_QUESTION;

    if (deciding) {
        set_act_slot(0, LV_SYMBOL_OK, COLOR_DONE,
                     (choices & PANEL_CHOICE_ALLOW_ONCE) != 0);
        set_act_slot(1, LV_SYMBOL_CLOSE, COLOR_PENDING,
                     (choices & PANEL_CHOICE_DENY) != 0);
        set_act_slot(2, LV_SYMBOL_SETTINGS, COLOR_WORKING,
                     (choices & PANEL_CHOICE_ALLOW_ALWAYS) != 0);
        s_act_action[0] = PANEL_ACT_ALLOW_ONCE;
        s_act_action[1] = PANEL_ACT_DENY;
        s_act_action[2] = PANEL_ACT_ALLOW_ALWAYS;
        lv_label_set_text(s_det_act_caption, "点击图标确认操作");
    } else {
        set_act_slot(0, LV_SYMBOL_PLAY, COLOR_ACCENT,
                     (choices & PANEL_CHOICE_CONTINUE) != 0);
        set_act_slot(1, LV_SYMBOL_CLOSE, COLOR_PENDING, false);
        set_act_slot(2, LV_SYMBOL_REFRESH, COLOR_WORKING,
                     (choices & PANEL_CHOICE_ALLOW_ALWAYS) != 0);
        s_act_action[0] = PANEL_ACT_CONTINUE;
        s_act_action[1] = PANEL_ACT_STOP;       /* no gateway action yet */
        s_act_action[2] = PANEL_ACT_ALLOW_ALWAYS;
        lv_label_set_text(s_det_act_caption, "停止请在主机按 Esc");
    }
    (void)online;
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
        dim_act_slots();
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

    /* content: pending card first, else output lines.
     *
     * Rows are laid out by the container's flex column, not by absolute
     * lv_obj_set_pos(). Absolute positioning needed a guessed height per
     * label; without one, LVGL left every label at 0x0 / (-1,-1) and drew
     * nothing at all, which is why CJK text looked blank here while Latin
     * text elsewhere rendered fine (the glyph data was always correct). */
    lv_obj_clean(s_det_content);
    lv_obj_set_flex_flow(s_det_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_det_content, 8, 0);
    lv_obj_set_style_pad_all(s_det_content, 12, 0);
    lv_obj_set_scroll_dir(s_det_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_det_content, LV_SCROLLBAR_MODE_AUTO);

    if (det->pending.kind != PANEL_PENDING_NONE && det->pending.summary[0] != '\0') {
        lv_obj_t *sum = lv_label_create(s_det_content);
        lv_label_set_long_mode(sum, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sum, 424);
        lv_label_set_text(sum, det->pending.summary);
        lv_obj_set_style_text_font(sum, ui_font(20), 0);
        lv_obj_set_style_text_color(sum, lv_color_hex(COLOR_TEXT), 0);

        if (det->pending.impact[0] != '\0') {
            lv_obj_t *imp = lv_label_create(s_det_content);
            lv_label_set_long_mode(imp, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(imp, 424);
            lv_label_set_text(imp, det->pending.impact);
            lv_obj_set_style_text_font(imp, ui_font(18), 0);
            lv_obj_set_style_text_color(imp, lv_color_hex(COLOR_DIM), 0);
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
        }
    } else {
        for (int i = 0; i < det->output_line_count; i++) {
            lv_obj_t *ln = lv_label_create(s_det_content);
            lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
            lv_obj_set_width(ln, 400);
            lv_obj_set_height(ln, 20);   /* fixed: LONG_DOT requires a height */
            lv_label_set_text(ln, det->output_lines[i]);
            lv_obj_set_style_text_font(ln, ui_font(16), 0);
            lv_obj_set_style_text_color(ln, lv_color_hex(COLOR_DIM), 0);
        }
        if (det->output_line_count == 0) {
            ui_label(s_det_content, 0, 0, 400, "(no output)", 18, COLOR_DISABLED);
        }
    }

    log_detail_layout(s_det_content, "det_content");

    /* action visibility from gateway choices only; actions need fresh data:
     * spec §4.3 — >6 s marks stale, >10 s disables actions entirely */
    bool online = (conn == PANEL_CONN_ONLINE);
    bool fresh = det->valid && online && age_s <= 10;
    uint8_t choices = fresh ? det->pending.choices : 0;

    if (panel_store_action_reserved()) {
        dim_act_slots();
        lv_label_set_text(s_det_hint, "操作进行中，请等待");
    } else if (online && age_s > 10) {
        dim_act_slots();
        lv_label_set_text(s_det_hint, "数据已过期，正在刷新");
    } else {
        render_action_slots(det, choices, online);
        if (!online) {
            lv_label_set_text(s_det_hint, "重连后才能操作");
        } else if (det->pending.kind == PANEL_PENDING_UNRECOGNIZED ||
                   det->herdr_status == PANEL_AGENT_BLOCKED) {
            lv_label_set_text(s_det_hint, "无法识别请求，请在主机处理");
        } else if (age_s > 6) {
            lv_label_set_text(s_det_hint, "数据可能已过期");
        } else {
            lv_label_set_text(s_det_hint, "操作前请确认");
        }
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
    ui_label(s_settings, 80, 20, 300, "Settings", 24, COLOR_TEXT);

    lv_obj_t *list = lv_obj_create(s_settings);
    lv_obj_set_pos(list, 8, 72);
    lv_obj_set_size(list, 464, 338);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    static const struct { const char *name; screen_t target; } items[] = {
        { "Sound", SCR_SOUND }, { "Display", SCR_DISPLAY },
        { "Sessions", SCR_SESSIONS }, { "Connection", SCR_CONNECTION },
        { "About", SCR_ABOUT }, { "Reprovision", SCR_REPROVISION_CONFIRM },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = ui_button(list, 8, i * 78, 448, 72, items[i].name,
                                i == 5 ? COLOR_PENDING : COLOR_CARD, 22);
        lv_obj_add_event_cb(b, on_settings_nav, LV_EVENT_CLICKED,
                            (void *)(intptr_t)items[i].target);
    }
    lv_obj_t *home = ui_button(s_settings, 16, 424, 448, 48, "Back to Home", COLOR_CARD, 18);
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

static lv_obj_t *add_pref_button_h(lv_obj_t *scr, int y, int h,
                                   const char *name, panel_pref_field_t field)
{
    lv_obj_t *b = ui_button(scr, 16, y, 448, h, name, COLOR_CARD, 20);
    lv_obj_add_event_cb(b, on_pref_click, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(field + 1));
    if (s_pref_widget_count < (int)(sizeof(s_pref_widgets) / sizeof(s_pref_widgets[0]))) {
        s_pref_widgets[s_pref_widget_count++] = (pref_widget_t){ b, field, name };
    }
    return b;
}

static lv_obj_t *add_pref_button(lv_obj_t *scr, int y,
                                  const char *name, panel_pref_field_t field)
{
    return add_pref_button_h(scr, y, 68, name, field);
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

/* Language policy (UI_DESIGN.md §2): session-facing screens (home grid,
 * detail, confirm, result) stay Chinese because they carry session and agent
 * status text. Settings screens are English-only so they never depend on the
 * generated CJK font cut, which keeps them legible on a truncated subset. */
static void build_pref_pages(void)
{
    s_quick = make_pref_page("Quick Settings", SCR_HOME);
    add_pref_button(s_quick, 80, "Sound", PANEL_PREF_SOUND_ENABLED);
    s_quick_brightness = add_pref_slider(s_quick, 174, "Brightness", PANEL_PREF_BRIGHTNESS, 10, 100);
    lv_obj_t *all = ui_button(s_quick, 16, 292, 448, 80, "All Settings", COLOR_ACCENT, 22);
    lv_obj_add_event_cb(all, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_SETTINGS);

    s_sound = make_pref_page("Sound", SCR_SETTINGS);
    s_sound_status = s_pref_status[s_pref_status_count - 1];
    add_pref_button(s_sound, 76, "Sound", PANEL_PREF_SOUND_ENABLED);
    s_sound_volume = add_pref_slider(s_sound, 156, "Volume", PANEL_PREF_SOUND_VOLUME, 0, 100);
    lv_obj_t *test_request = ui_button(s_sound, 16, 215, 214, 36,
                                       "Test Input", COLOR_BORDER, 16);
    lv_obj_t *test_done = ui_button(s_sound, 250, 215, 214, 36,
                                    "Test Done", COLOR_BORDER, 16);
    lv_obj_add_event_cb(test_request, on_sound_test, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PANEL_AUDIO_REQUEST);
    lv_obj_add_event_cb(test_done, on_sound_test, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PANEL_AUDIO_DONE);
    add_pref_button(s_sound, 256, "On Input", PANEL_PREF_SOUND_REQUEST);
    add_pref_button(s_sound, 330, "On Done", PANEL_PREF_SOUND_DONE);
    lv_obj_t *more = ui_button(s_sound, 16, 404, 448, 36, "More Sound Options", COLOR_BORDER, 16);
    lv_obj_add_event_cb(more, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_SOUND_MORE);

    s_sound_more = make_pref_page("Sound Options", SCR_SOUND);
    add_pref_button(s_sound_more, 76, "Alert Scope", PANEL_PREF_SOUND_SCOPE);
    add_pref_button(s_sound_more, 150, "Claude", PANEL_PREF_SOUND_CLAUDE);
    add_pref_button(s_sound_more, 224, "OpenCode", PANEL_PREF_SOUND_OPENCODE);
    add_pref_button(s_sound_more, 298, "Pi", PANEL_PREF_SOUND_PI);
    lv_obj_t *quiet = ui_button(s_sound_more, 16, 372, 448, 68, "Quiet Hours", COLOR_CARD, 20);
    lv_obj_add_event_cb(quiet, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_QUIET);

    s_quiet = make_pref_page("Quiet Hours", SCR_SOUND_MORE);
    add_pref_button(s_quiet, 76, "Enabled", PANEL_PREF_QUIET_ENABLED);
    add_pref_button(s_quiet, 150, "Start (+15 min)", PANEL_PREF_QUIET_START);
    add_pref_button(s_quiet, 224, "End (+15 min)", PANEL_PREF_QUIET_END);
    add_pref_button(s_quiet, 298, "UTC Offset (+15 min)", PANEL_PREF_UTC_OFFSET);
    s_quiet_note = ui_label(s_quiet, 20, 382, 440, "", 16, COLOR_PENDING);

    s_display = make_pref_page("Display", SCR_SETTINGS);
    s_display_brightness = add_pref_slider(s_display, 68, "Brightness", PANEL_PREF_BRIGHTNESS, 10, 100);
    /* Two new rows replaced the single "Idle Dimming" row, so the buttons go
     * to 58 px to fit all six plus the slider inside 480 px. */
    add_pref_button_h(s_display, 152, 58, "Standby Clock", PANEL_PREF_IDLE_DISPLAY_SECONDS);
    add_pref_button_h(s_display, 214, 58, "Blank Screen", PANEL_PREF_IDLE_BLANK_SECONDS);
    add_pref_button_h(s_display, 276, 58, "Wake on Motion", PANEL_PREF_MOTION_WAKE);
    add_pref_button_h(s_display, 338, 58, "Dimmed Level", PANEL_PREF_DIM_BRIGHTNESS);
    add_pref_button_h(s_display, 400, 58, "Reduce Motion", PANEL_PREF_REDUCE_MOTION);

    s_sessions = make_pref_page("Sessions", SCR_SETTINGS);
    add_pref_button(s_sessions, 76, "Refresh Interval", PANEL_PREF_OVERVIEW_INTERVAL);
    add_pref_button(s_sessions, 150, "Card Order", PANEL_PREF_CARD_ORDER);
    add_pref_button(s_sessions, 224, "Hide Idle", PANEL_PREF_HIDE_IDLE);
    /* Lived on the Display page until standby took two of its rows. */
    add_pref_button(s_sessions, 298, "Visual Alerts", PANEL_PREF_VISUAL_ALERT);
    app_config_t session_cfg;
    app_config_get(&session_cfg);
    char prompt_summary[224];
    snprintf(prompt_summary, sizeof(prompt_summary), "Continue prompt: %s",
             session_cfg.continue_prompt[0] ? session_cfg.continue_prompt : "Gateway default");
    ui_label(s_sessions, 24, 380, 432, prompt_summary, 18, COLOR_DIM);
    lv_obj_t *edit_prompt = ui_button(s_sessions, 16, 424, 448, 68,
                                      "Scan to Edit Continue Prompt", COLOR_CARD, 20);
    lv_obj_add_event_cb(edit_prompt, on_connection_edit, LV_EVENT_CLICKED, NULL);

    s_connection = make_pref_page("Connection", SCR_SETTINGS);
    app_config_t cfg;
    app_config_get(&cfg);
    char label[180];
    snprintf(label, sizeof(label), "Wi-Fi: %s\nGateway: %s:%u",
             cfg.wifi_ssid, cfg.backend_host, (unsigned)cfg.backend_port);
    lv_obj_t *info = ui_label(s_connection, 20, 100, 440, label, 20, COLOR_TEXT);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(info, 160);
    s_connection_power = ui_label(s_connection, 20, 235, 440,
                                  "Battery: reading...", 18, COLOR_DIM);
    lv_obj_t *refresh = ui_button(s_connection, 16, 285, 448, 66, "Refresh Connection", COLOR_CARD, 20);
    lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, NULL);
    lv_obj_t *edit_connection = ui_button(s_connection, 16, 365, 448, 66,
                                          "Scan to Edit Connection", COLOR_ACCENT, 20);
    lv_obj_add_event_cb(edit_connection, on_connection_edit, LV_EVENT_CLICKED, NULL);

    s_about = make_pref_page("About", SCR_SETTINGS);
    ui_label(s_about, 20, 110, 440, "Herdr Panel\nESP-IDF · Panel API v1", 20, COLOR_TEXT);

    s_reprovision = make_pref_page("Reprovision?", SCR_SETTINGS);
    lv_obj_t *warn = ui_label(s_reprovision, 20, 110, 440,
                              "Clear Wi-Fi and gateway token?\nDisplay and sound settings are kept.",
                              20, COLOR_TEXT);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(warn, 120);
    lv_obj_t *cancel = ui_button(s_reprovision, 16, 286, 448, 64, "Cancel", COLOR_CARD, 20);
    lv_obj_add_event_cb(cancel, on_settings_nav, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCR_SETTINGS);
    lv_obj_t *confirm = ui_button(s_reprovision, 16, 366, 448, 70,
                                  "Clear Credentials", COLOR_PENDING, 20);
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
            value = v == PANEL_SOUND_PAGE ? "This Page" :
                    v == PANEL_SOUND_SELECTED ? "Selected" : "All";
        } else if (w->field == PANEL_PREF_CARD_ORDER) {
            value = v == PANEL_ORDER_BLOCKED ? "Blocked First" :
                    v == PANEL_ORDER_RECENT ? "Recently Updated" : "Fixed Order";
        } else if (w->field == PANEL_PREF_SOUND_CLAUDE ||
                   w->field == PANEL_PREF_SOUND_OPENCODE ||
                   w->field == PANEL_PREF_SOUND_PI) {
            value = v == PANEL_SOUND_ON ? "On" :
                    v == PANEL_SOUND_OFF ? "Off" : "Inherit";
        } else if (w->field == PANEL_PREF_QUIET_START || w->field == PANEL_PREF_QUIET_END) {
            snprintf(buf, sizeof(buf), "%s  %02d:%02d", w->name, v / 60, v % 60);
        } else if (w->field == PANEL_PREF_UTC_OFFSET) {
            int absv = v < 0 ? -v : v;
            snprintf(buf, sizeof(buf), "%s  %c%02d:%02d", w->name,
                     v < 0 ? '-' : '+', absv / 60, absv % 60);
        } else if (w->field == PANEL_PREF_OVERVIEW_INTERVAL) {
            snprintf(buf, sizeof(buf), "%s  %d s", w->name, v);
        } else if (w->field == PANEL_PREF_IDLE_DISPLAY_SECONDS) {
            snprintf(buf, sizeof(buf), "%s  %s", w->name,
                     v == 0 ? "Off" : standby_duration(v));
        } else if (w->field == PANEL_PREF_IDLE_BLANK_SECONDS) {
            snprintf(buf, sizeof(buf), "%s  %s", w->name,
                     v == 0 ? "Never" : standby_duration(v));
        } else if (w->field == PANEL_PREF_MOTION_WAKE) {
            value = panel_motion_present() ? (v ? "On" : "Off")
                                           : "No IMU";
        } else if (w->field == PANEL_PREF_DIM_BRIGHTNESS) {
            snprintf(buf, sizeof(buf), "%s  %d%%", w->name, v);
        } else {
            value = v ? "On" : "Off";
        }
        if (value != NULL) snprintf(buf, sizeof(buf), "%s  %s", w->name, value);
        lv_label_set_text(lv_obj_get_child(w->button, 0), buf);
    }
    if (s_quick_brightness) lv_slider_set_value(s_quick_brightness, p.brightness, LV_ANIM_OFF);
    if (s_display_brightness) lv_slider_set_value(s_display_brightness, p.brightness, LV_ANIM_OFF);
    if (s_sound_volume) lv_slider_set_value(s_sound_volume, p.sound_volume, LV_ANIM_OFF);
    if (s_quiet_note) {
        const char *note = p.quiet_enabled && time(NULL) < 1704067200 ?
            "Clock not synced; sound muted" :
            p.quiet_enabled && p.quiet_start == p.quiet_end ?
            "Same start and end: muted all day" :
            "Fixed UTC offset; adjust for daylight saving";
        lv_label_set_text(s_quiet_note, note);
    }
}

/* ====================================================================== */
/* PRV                                                                     */
/* ====================================================================== */

static void build_provision(void)
{
    s_prov = make_screen();
    ui_label(s_prov, 16, 24, 360, "连接设备热点", 28, COLOR_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(s_prov, 0), LV_TEXT_ALIGN_CENTER, 0);
    s_prov_cancel = ui_button(s_prov, 386, 16, 78, 52, "取消", COLOR_CARD, 18);
    lv_obj_add_event_cb(s_prov_cancel, on_cancel_edit, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_prov_cancel, LV_OBJ_FLAG_HIDDEN);

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

    s_prov_remaining_last = -2;
    int remaining = provisioning_remaining_seconds();
    if (remaining >= 0) lv_obj_clear_flag(s_prov_cancel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_prov_cancel, LV_OBJ_FLAG_HIDDEN);
    char info[192];
    if (remaining >= 0)
        snprintf(info, sizeof(info),
                 "SSID：%s\n密码：%s\n网页：192.168.4.1\n编辑剩余 %02d:%02d",
                 s_prov_ssid, s_prov_pass, remaining / 60, remaining % 60);
    else
        snprintf(info, sizeof(info),
                 "SSID：%s\n密码：%s\n扫码连接 Wi-Fi\n网页：192.168.4.1",
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
    case SCR_STANDBY: scr = s_standby; break;
    }
    if (scr != NULL) {
        panel_prefs_t p;
        panel_prefs_get(&p);
        if (previous != s && previous != SCR_BOOT && s != SCR_BOOT && !p.reduce_motion &&
            s != SCR_CONFIRM && s != SCR_PROVISION) {
            lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, false);
        } else {
            lv_screen_load(scr);
        }
        if (previous == SCR_BOOT && s != SCR_BOOT && s_boot != NULL) {
            lv_obj_delete(s_boot);
            s_boot = NULL;
            s_boot_progress = NULL;
            for (int i = 0; i < 4; i++) s_boot_cells[i] = NULL;
            log_ui_memory("boot released");
        }
        if (s == SCR_SOUND && !panel_audio_ready()) {
            lv_label_set_text(s_sound_status, "声音不可用，请检查扬声器");
        }
    }
}

static void on_card(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= s_cell_count) return;
    const char *term = s_cells[slot].terminal_id;
    if (term[0] == '\0') return;

    bump_epoch();
    snprintf(s_sel_term, sizeof(s_sel_term), "%s", term);
    panel_store_set_view_context(s_sel_term);

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
    dim_act_slots();

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
                lv_label_set_text(s_pref_status[i], "Failed to clear credentials");
            }
        }
        return;
    }
    if (s_screen == SCR_HOME) {
        refresh_prefs_ui();
        ensure_settings_pages(SCR_QUICK);
        show_screen(SCR_QUICK);
    }
}

static void on_connection_edit(lv_event_t *e)
{
    (void)e;
    panel_cmd_t cmd = { .type = PANEL_CMD_EDIT_CONNECTION };
    const char *msg = panel_store_enqueue_control(&cmd) ?
                      "Opening edit hotspot…" : "Busy, try again";
    for (int i = 0; i < s_pref_status_count; i++)
        lv_label_set_text(s_pref_status[i], msg);
}

static void on_cancel_edit(lv_event_t *e)
{
    (void)e;
    if (provisioning_remaining_seconds() >= 0) esp_restart();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (s_screen == SCR_DETAIL) {
        bump_epoch();
        s_sel_term[0] = '\0';
        panel_store_set_view_context(NULL);
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

/* Settings pages are built on first use, not at startup.
 *
 * All sixteen screens used to be instantiated during ui_panel_init(), which
 * left the 96 KB LVGL heap at 15.5 KB free (83% used, see the "pages built"
 * and "provision built" checkpoints). Opening the settings menu then had no
 * room left to lay out and render, so the screen came up blank. Building the
 * settings pages on demand keeps the home screen's headroom and only spends
 * heap on pages the user actually visits. */
static void build_pref_pages(void);

static bool settings_page_built(screen_t s)
{
    switch (s) {
    case SCR_QUICK:               return s_quick != NULL;
    case SCR_SOUND:               return s_sound != NULL;
    case SCR_SOUND_MORE:          return s_sound_more != NULL;
    case SCR_QUIET:               return s_quiet != NULL;
    case SCR_DISPLAY:             return s_display != NULL;
    case SCR_SESSIONS:            return s_sessions != NULL;
    case SCR_CONNECTION:          return s_connection != NULL;
    case SCR_ABOUT:               return s_about != NULL;
    case SCR_REPROVISION_CONFIRM: return s_reprovision != NULL;
    default:                      return true;
    }
}

static void ensure_settings_pages(screen_t target)
{
    /* SCR_SETTINGS itself stays built at startup: it is only six buttons and
     * is the entry point for every settings page. */
    if (target == SCR_SETTINGS) return;

    if (!settings_page_built(target)) {
        build_pref_pages();
        log_ui_memory("settings pages built");
    }
    /* 声音 and 声音选项 reference each other through their back buttons;
     * both are covered by the single build_pref_pages() pass above. */
}

static void on_settings_nav(lv_event_t *e)
{
    screen_t target = (screen_t)(intptr_t)lv_event_get_user_data(e);
    refresh_prefs_ui();
    ensure_settings_pages(target);
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
            lv_label_set_text(s_pref_status[i], "Busy; setting not saved");
        }
        panel_prefs_t p;
        panel_prefs_get(&p);
        panel_display_set_brightness(p.brightness);
        panel_audio_set_volume(p.sound_volume);
        refresh_prefs_ui();
        return false;
    }
    for (int i = 0; i < s_pref_status_count; i++) {
        lv_label_set_text(s_pref_status[i], "Saving…");
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
    case PANEL_PREF_MOTION_WAKE:
        /* No IMU means the toggle would silently do nothing, so say so
         * instead of storing a preference that cannot be honoured. */
        if (!panel_motion_present()) {
            for (int i = 0; i < s_pref_status_count; i++)
                lv_label_set_text(s_pref_status[i],
                                  "No IMU on this board; touch still wakes");
            return;
        }
        next = !v;
        break;
    case PANEL_PREF_SOUND_SCOPE: case PANEL_PREF_SOUND_CLAUDE:
    case PANEL_PREF_SOUND_OPENCODE: case PANEL_PREF_SOUND_PI:
    case PANEL_PREF_CARD_ORDER: next = (v + 1) % 3; break;
    case PANEL_PREF_QUIET_START: case PANEL_PREF_QUIET_END:
        next = (v + 15) % 1440; break;
    case PANEL_PREF_UTC_OFFSET: next = v >= 840 ? -720 : v + 15; break;
    case PANEL_PREF_IDLE_DISPLAY_SECONDS: next = standby_next(v); break;
    case PANEL_PREF_IDLE_BLANK_SECONDS:   next = standby_next(v); break;
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
        lv_label_set_text(s_sound_status, "Sound unavailable; check speaker");
    } else if (lv_slider_get_value(s_sound_volume) == 0) {
        lv_label_set_text(s_sound_status, "Raise the volume first");
    } else {
        bool queued = panel_audio_play((panel_audio_kind_t)(intptr_t)lv_event_get_user_data(e));
        lv_label_set_text(s_sound_status, queued ?
                          "Preview only; mute setting unchanged" : "Sound busy, try again");
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
    /* spec §2.2: tapping the blocked count scrolls the first blocked session
     * into view. With one list there is no page to jump to, so scroll the
     * container by the row offset instead. */
    int idx = panel_store_first_blocked_index();
    if (idx <= 0 || s_card_list == NULL) return;
    int row = idx < s_cell_count ? idx : s_cell_count - 1;
    lv_obj_scroll_to_view(lv_obj_get_child(s_card_list, row), LV_ANIM_ON);
}

/*
 * Swipe paging is gone (UI_DESIGN.md §4.1). The home screen is a single
 * scrolling list, so LVGL's own vertical scroll handling replaces both the
 * LV_EVENT_GESTURE hook and the manual press/press/release swipe detector.
 * A card tap therefore no longer races a page turn: once the list scrolls,
 * LVGL suppresses CLICKED on the card underneath.
 */

static void on_action_btn(lv_event_t *e)
{
    /* user_data is the slot index, not the action id; s_act_action[] is set by
     * render_action_slots() and holds PANEL_ACT_NONE for inert slots. */
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= 3) return;
    panel_action_id_t act = s_act_action[slot];
    /* STOP has no gateway Action literal yet and can never be delivered, so
     * the slot stays dimmed and any click on it is ignored here too. */
    if (act == PANEL_ACT_NONE || act == PANEL_ACT_STOP) return;

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
    log_font_probe();
    build_boot();
    log_ui_memory("boot built");
    s_boot_started_ms = ui_now_ms();
    show_screen(SCR_BOOT);
    /* Put a known frame on the panel before allocating every settings page.
     * If a later startup stage fails, the serial checkpoints and this frame
     * distinguish UI allocation from panel power or SPI failures. */
    lv_refr_now(NULL);
    ESP_LOGI("ui_panel", "boot frame flushed");
    build_home();
    build_detail();
    build_confirm();
    build_result();
    build_settings();
    /* The ten settings pages are built on first use (ensure_settings_pages)
     * to keep the LVGL heap available for the home screen. */
    log_ui_memory("pages built");
    build_provision();
    log_ui_memory("provision built");
    panel_store_set_view_context(NULL);
    refresh_prefs_ui();
    refresh_power_ui();
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
    if (s_screen == SCR_PROVISION) {
        int remaining = provisioning_remaining_seconds();
        if (remaining >= 0 && remaining != s_prov_remaining_last) {
            s_prov_remaining_last = remaining;
            char info[192];
            snprintf(info, sizeof(info),
                     "SSID：%s\n密码：%s\n网页：192.168.4.1\n编辑剩余 %02d:%02d",
                     s_prov_ssid, s_prov_pass, remaining / 60, remaining % 60);
            lv_label_set_text(s_prov_info, info);
        }
    }
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
        } else if (evt.type == PANEL_EVT_FATAL_ERROR) {
            for (int i = 0; i < s_pref_status_count; i++)
                lv_label_set_text(s_pref_status[i], evt.message);
        }
    }

    if (s_screen == SCR_CONFIRM && !confirmed_request_current()) {
        s_pending_action = PANEL_ACT_NONE;
        show_screen(SCR_DETAIL);
        lv_label_set_text(s_det_hint, "请求已变化或过期，请刷新后重看");
    }

    /* Standby replaces the old flat "dim after idle" behaviour: the clock
     * screen and the fully dark stage both own the brightness, so this runs
     * before the store refresh below, which may still be about to touch the
     * real screens. */
    update_standby();

    /* 2. redraw when store generation changed */
    uint32_t gen = panel_store_generation();
    bool gen_changed = (gen != s_seen_generation);
    s_seen_generation = gen;

    if (s_screen == SCR_HOME) {
        update_alert_pulses();
        if (!gen_changed) return;

        /* One list, no paging: copy the visible cards straight into the
         * static buffer (worker-owned snapshots stay off the LVGL stack). */
        static panel_agent_card_t cards[PANEL_MAX_AGENTS];
        int count = 0, total = 0;
        panel_conn_state_t conn;
        count = panel_store_get_visible(cards, PANEL_MAX_AGENTS, &total, &conn);
        s_card_count = count;
        panel_store_set_view_context(s_sel_term);

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
