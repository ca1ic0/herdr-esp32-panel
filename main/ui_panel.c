/*
 * LVGL UI for the Herdr panel.
 *
 * Home      : full-screen 2x2 grid of herdr sessions, swipe to page,
 *             one small settings gear in the bottom bar.
 * Detail    : session content + three action buttons  ✓ / ✗ / →
 * Settings  : WiFi / Backend / Reset-Network entries
 * Forms     : LVGL keyboard driven text input, save to NVS + reboot
 * Provision : QR code screen for first-time setup
 */

#include "ui_panel.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "herdr_model.h"
#include "panel_state.h"
#include "ui_common.h"

#define SCREEN_W        480
#define SCREEN_H        480

#define GRID_X0         8
#define GRID_X1         244
#define GRID_Y0         6
#define GRID_Y1         227
#define CELL_W          228
#define CELL_H          213
#define BAR_Y           440        /* slim bottom bar: dots + gear */

#define MAX_DOTS        8

typedef enum {
    SCR_HOME = 0,
    SCR_DETAIL,
    SCR_SETTINGS,
    SCR_WIFI_FORM,
    SCR_BACKEND_FORM,
    SCR_PROVISION,
} screen_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *dot;
    lv_obj_t *title;
    lv_obj_t *status;
    lv_obj_t *cwd;
    lv_obj_t *state;
    lv_obj_t *pane;
} cell_widgets_t;

/* --- widget handles ---------------------------------------------------- */

static lv_obj_t *s_home, *s_detail, *s_settings, *s_wifi_form, *s_backend_form,
                *s_provision;

static lv_obj_t *s_page_dots[MAX_DOTS];
static lv_obj_t *s_conn_label;
static lv_obj_t *s_empty_label;
static cell_widgets_t s_cells[4];

static lv_obj_t *s_det_title, *s_det_status, *s_det_cwd, *s_det_pane,
                *s_det_output, *s_det_toast;

static lv_obj_t *s_set_wifi_sub, *s_set_backend_sub;

static lv_obj_t *s_ta_ssid, *s_ta_pass, *s_kb_wifi;
static lv_obj_t *s_ta_host, *s_ta_port, *s_kb_backend;

static lv_obj_t *s_prov_qr, *s_prov_info;

/* --- local model -------------------------------------------------------- */

static herdr_session_t s_sessions[HERDR_MAX_SESSIONS];
static int s_count = 0;
static int s_page = 0;
static int s_selected = -1;
static screen_t s_screen = SCR_HOME;
static uint32_t s_last_output_fetch = 0;

/* forward declarations for callbacks used while building widgets */
static void on_card_clicked(lv_event_t *e);
static void on_home_gesture(lv_event_t *e);
static void on_action(lv_event_t *e);
static void on_gear(lv_event_t *e);
static void on_back_home(lv_event_t *e);
static void on_back_settings(lv_event_t *e);
static void on_open_wifi(lv_event_t *e);
static void on_open_backend(lv_event_t *e);
static void on_reset_network(lv_event_t *e);
static void on_save_wifi(lv_event_t *e);
static void on_save_backend(lv_event_t *e);
static void on_ta_focus(lv_event_t *e);

/* --- small helpers ------------------------------------------------------ */

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

static void restart_soon(void)
{
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *txt, uint32_t bg, int font_size)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, ui_font(font_size), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *make_textarea(lv_obj_t *parent, int x, int y, int w,
                               const char *placeholder, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, 44);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_max_length(ta, 64);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, ui_font(16), 0);
    return ta;
}

/* --- home grid ---------------------------------------------------------- */

static void build_cells(void)
{
    static const int col_x[2] = { GRID_X0, GRID_X1 };
    static const int row_y[2] = { GRID_Y0, GRID_Y1 };

    for (int i = 0; i < 4; i++) {
        cell_widgets_t *c = &s_cells[i];

        c->card = lv_obj_create(s_home);
        lv_obj_set_pos(c->card, col_x[i % 2], row_y[i / 2]);
        lv_obj_set_size(c->card, CELL_W, CELL_H);
        lv_obj_set_style_bg_color(c->card, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c->card, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_border_width(c->card, 1, 0);
        lv_obj_set_style_radius(c->card, 14, 0);
        lv_obj_set_style_pad_all(c->card, 0, 0);
        lv_obj_set_scrollable(c->card, false);
        lv_obj_set_clickable(c->card, true);
        lv_obj_add_event_cb(c->card, on_card_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        c->dot = lv_obj_create(c->card);
        lv_obj_set_pos(c->dot, 12, 16);
        lv_obj_set_size(c->dot, 14, 14);
        lv_obj_set_style_radius(c->dot, 7, 0);
        lv_obj_set_style_bg_color(c->dot, lv_color_hex(0x4a5563), 0);
        lv_obj_set_style_bg_opa(c->dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c->dot, 0, 0);
        lv_obj_set_scrollable(c->dot, false);

        c->title = ui_label(c->card, 34, 8, CELL_W - 46, "---", 16, COLOR_TEXT);
        c->status = ui_label(c->card, 12, 42, CELL_W - 24, "UNKNOWN", 14, 0x4a5563);
        c->cwd = ui_label(c->card, 12, 72, CELL_W - 24, "", 12, COLOR_DIM);
        c->state = ui_label(c->card, 12, 100, CELL_W - 24, "", 12, COLOR_ACCENT);
        c->pane = ui_label(c->card, 12, CELL_H - 30, CELL_W - 24, "", 12, COLOR_DIM);
    }
}

static void fill_cell(int slot, const herdr_session_t *s)
{
    cell_widgets_t *c = &s_cells[slot];
    uint32_t color = herdr_status_color(s->status);

    lv_obj_set_hidden(c->card, false);
    lv_obj_set_style_border_color(c->card,
                                  lv_color_hex(s->focused ? COLOR_ACCENT : COLOR_BORDER), 0);
    lv_obj_set_style_border_width(c->card, s->focused ? 2 : 1, 0);

    lv_obj_set_style_bg_color(c->dot, lv_color_hex(color), 0);
    lv_label_set_text(c->title, s->title);
    lv_label_set_text(c->status, herdr_status_name(s->status));
    lv_obj_set_style_text_color(c->status, lv_color_hex(color), 0);

    lv_label_set_text(c->cwd, s->cwd);
    lv_label_set_text(c->state, s->state);
    lv_label_set_text(c->pane, s->pane_id);
}

static void refresh_dots(int pages)
{
    for (int i = 0; i < MAX_DOTS; i++) {
        if (s_page_dots[i] == NULL) {
            continue;
        }
        if (i >= pages) {
            lv_obj_set_hidden(s_page_dots[i], true);
            continue;
        }
        lv_obj_set_hidden(s_page_dots[i], false);
        lv_obj_set_style_bg_color(s_page_dots[i],
                                  lv_color_hex(i == s_page ? COLOR_ACCENT : COLOR_BORDER), 0);
    }
}

static void refresh_grid(void)
{
    int pages = (s_count + 3) / 4;
    if (pages < 1) {
        pages = 1;
    }
    if (s_page >= pages) {
        s_page = pages - 1;
    }
    if (s_page < 0) {
        s_page = 0;
    }

    for (int slot = 0; slot < 4; slot++) {
        int idx = s_page * 4 + slot;
        if (idx < s_count) {
            fill_cell(slot, &s_sessions[idx]);
        } else {
            lv_obj_set_hidden(s_cells[slot].card, true);
        }
    }

    lv_obj_set_hidden(s_empty_label, s_count != 0);
    refresh_dots(pages);
}

/* --- navigation --------------------------------------------------------- */

static void goto_detail(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return;
    }
    s_selected = idx;
    s_screen = SCR_DETAIL;

    const herdr_session_t *s = &s_sessions[idx];
    lv_label_set_text(s_det_title, s->title);
    lv_label_set_text(s_det_status, herdr_status_name(s->status));
    lv_obj_set_style_text_color(s_det_status,
                                lv_color_hex(herdr_status_color(s->status)), 0);
    lv_label_set_text(s_det_cwd, s->cwd);
    lv_label_set_text(s_det_pane, s->pane_id);
    lv_label_set_text(s_det_output, "(loading output...)");
    lv_label_set_text(s_det_toast, "");

    panel_set_detail_pane(s->pane_id);
    panel_fetch_output(s->pane_id);
    s_last_output_fetch = lv_tick_get();

    lv_screen_load(s_detail);
}

static void goto_home(void)
{
    s_screen = SCR_HOME;
    s_selected = -1;
    panel_set_detail_pane("");
    lv_screen_load(s_home);
    refresh_grid();
}

static void goto_screen(lv_obj_t *scr, screen_t id)
{
    s_screen = id;
    lv_screen_load(scr);
}

static void on_card_clicked(lv_event_t *e)
{
    intptr_t slot = (intptr_t)lv_event_get_user_data(e);
    goto_detail(s_page * 4 + (int)slot);
}

static void on_home_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT && (s_page + 1) * 4 < s_count) {
        s_page++;
        refresh_grid();
    } else if (dir == LV_DIR_RIGHT && s_page > 0) {
        s_page--;
        refresh_grid();
    }
    (void)e;
}

static void on_back_home(lv_event_t *e)
{
    (void)e;
    goto_home();
}

static void on_gear(lv_event_t *e)
{
    (void)e;
    /* refresh the settings summary lines */
    app_config_t cfg;
    app_config_get(&cfg);
    lv_label_set_text(s_set_wifi_sub, cfg.wifi_ssid);
    lv_label_set_text(s_set_backend_sub, cfg.backend_host);
    goto_screen(s_settings, SCR_SETTINGS);
}

static void on_back_settings(lv_event_t *e)
{
    (void)e;
    if (s_screen == SCR_WIFI_FORM || s_screen == SCR_BACKEND_FORM) {
        goto_screen(s_settings, SCR_SETTINGS);
    } else {
        goto_home();
    }
}

static void on_action(lv_event_t *e)
{
    if (s_selected < 0 || s_selected >= s_count) {
        return;
    }
    const char *payload = (const char *)lv_event_get_user_data(e);
    const herdr_session_t *s = &s_sessions[s_selected];

    if (panel_send_text(s->pane_id, payload)) {
        lv_label_set_text_fmt(s_det_toast, "sent: %s", payload);
    } else {
        lv_label_set_text(s_det_toast, "queue full, try again");
    }
    panel_fetch_output(s->pane_id);
    s_last_output_fetch = lv_tick_get();
}

/* --- settings forms ------------------------------------------------------ */

static void on_open_wifi(lv_event_t *e)
{
    (void)e;
    app_config_t cfg;
    app_config_get(&cfg);
    lv_textarea_set_text(s_ta_ssid, cfg.wifi_ssid);
    lv_textarea_set_text(s_ta_pass, cfg.wifi_pass);
    lv_keyboard_set_textarea(s_kb_wifi, s_ta_ssid);
    goto_screen(s_wifi_form, SCR_WIFI_FORM);
}

static void on_open_backend(lv_event_t *e)
{
    (void)e;
    app_config_t cfg;
    app_config_get(&cfg);
    lv_textarea_set_text(s_ta_host, cfg.backend_host);
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)cfg.backend_port);
    lv_textarea_set_text(s_ta_port, port);
    lv_keyboard_set_textarea(s_kb_backend, s_ta_host);
    goto_screen(s_backend_form, SCR_BACKEND_FORM);
}

static void on_reset_network(lv_event_t *e)
{
    (void)e;
    app_config_t cfg;
    app_config_get(&cfg);
    cfg.wifi_ssid[0] = '\0';
    cfg.wifi_pass[0] = '\0';
    app_config_save(&cfg);
    restart_soon();
}

static void on_save_wifi(lv_event_t *e)
{
    (void)e;
    app_config_t cfg;
    app_config_get(&cfg);
    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s",
             lv_textarea_get_text(s_ta_ssid));
    snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s",
             lv_textarea_get_text(s_ta_pass));
    app_config_save(&cfg);
    restart_soon();
}

static void on_save_backend(lv_event_t *e)
{
    (void)e;
    app_config_t cfg;
    app_config_get(&cfg);
    snprintf(cfg.backend_host, sizeof(cfg.backend_host), "%s",
             lv_textarea_get_text(s_ta_host));
    int port = atoi(lv_textarea_get_text(s_ta_port));
    if (port > 0 && port < 65536) {
        cfg.backend_port = (uint16_t)port;
    }
    app_config_save(&cfg);
    restart_soon();
}

static void on_ta_focus(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    lv_keyboard_set_textarea(
        (s_screen == SCR_WIFI_FORM) ? s_kb_wifi : s_kb_backend, ta);
}

/* --- screen construction -------------------------------------------------- */

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);
    /* the default theme pads lv_obj by ~20px; children use absolute
     * coordinates, so padding must be zeroed or they overflow the screen */
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_set_style_pad_column(scr, 0, 0);
    return scr;
}

static lv_obj_t *make_back_button(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, 8, 8);
    lv_obj_set_size(btn, 72, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " back");
    lv_obj_set_style_text_font(lbl, ui_font(14), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void build_home(void)
{
    s_home = new_screen();
    lv_obj_add_event_cb(s_home, on_home_gesture, LV_EVENT_GESTURE, NULL);

    build_cells();

    s_empty_label = ui_label(s_home, 0, 200, SCREEN_W,
                             "no active herdr sessions", 16, COLOR_DIM);
    lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);

    /* slim bottom bar: page dots (center) + settings gear (right) */
    lv_obj_t *bar = lv_obj_create(s_home);
    lv_obj_set_pos(bar, 0, BAR_Y);
    lv_obj_set_size(bar, SCREEN_W, SCREEN_H - BAR_Y);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_ELEVATED), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_pad_all(bar, 0, 0);

    s_conn_label = ui_label(bar, 12, 12, 200, "", 12, COLOR_DIM);

    int dot_total_w = MAX_DOTS * 18;
    for (int i = 0; i < MAX_DOTS; i++) {
        lv_obj_t *d = lv_obj_create(bar);
        lv_obj_set_pos(d, (SCREEN_W - dot_total_w) / 2 + i * 18, 16);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, 5, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_scrollable(d, false);
        s_page_dots[i] = d;
    }

    lv_obj_t *gear = lv_button_create(bar);
    lv_obj_set_pos(gear, SCREEN_W - 60, 4);
    lv_obj_set_size(gear, 52, 32);
    lv_obj_set_style_bg_color(gear, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(gear, 10, 0);
    lv_obj_t *lbl = lv_label_create(gear);
    lv_label_set_text(lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(lbl, ui_font(20), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(gear, on_gear, LV_EVENT_CLICKED, NULL);
}

static void build_detail(void)
{
    s_detail = new_screen();

    make_back_button(s_detail, on_back_home);
    s_det_title = ui_label(s_detail, 88, 12, 280, "---", 16, COLOR_TEXT);
    s_det_status = ui_label(s_detail, 370, 14, 98, "UNKNOWN", 12, COLOR_DIM);
    lv_obj_set_style_text_align(s_det_status, LV_TEXT_ALIGN_RIGHT, 0);
    s_det_cwd = ui_label(s_detail, 12, 52, 456, "", 12, COLOR_DIM);
    s_det_pane = ui_label(s_detail, 12, 74, 456, "", 12, COLOR_DIM);

    lv_obj_t *box = lv_obj_create(s_detail);
    lv_obj_set_pos(box, 12, 100);
    lv_obj_set_size(box, 456, 230);
    lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);

    s_det_output = lv_label_create(box);
    lv_obj_set_width(s_det_output, 436);
    lv_label_set_long_mode(s_det_output, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_det_output, "(no output)");
    lv_obj_set_style_text_font(s_det_output, ui_font(12), 0);
    lv_obj_set_style_text_color(s_det_output, lv_color_hex(COLOR_TEXT), 0);

    s_det_toast = ui_label(s_detail, 12, 338, 456, "", 12, COLOR_ACCENT);

    /* three action buttons: ✓ allow   ✗ deny   → continue */
    struct {
        const char *sym;
        const char *cap;
        uint32_t color;
        const char *payload;
    } acts[3] = {
        { LV_SYMBOL_OK,    "allow",    COLOR_GREEN, CONFIG_HERDR_ACTION_ALLOW_TEXT },
        { LV_SYMBOL_CLOSE, "deny",     COLOR_RED,   CONFIG_HERDR_ACTION_DENY_TEXT },
        { LV_SYMBOL_RIGHT, "continue", COLOR_ACCENT, CONFIG_HERDR_ACTION_CONTINUE_TEXT },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(s_detail);
        lv_obj_set_pos(btn, 12 + i * 156, 364);
        lv_obj_set_size(btn, 144, 76);
        lv_obj_set_style_bg_color(btn, lv_color_hex(acts[i].color), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_add_event_cb(btn, on_action, LV_EVENT_CLICKED, (void *)acts[i].payload);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "%s\n%s", acts[i].sym, acts[i].cap);
        lv_obj_set_style_text_font(lbl, ui_font(20), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(lbl, 2, 0);
        lv_obj_center(lbl);
    }
}

static void build_settings(void)
{
    s_settings = new_screen();
    make_back_button(s_settings, on_back_settings);
    ui_label(s_settings, 88, 12, 300, "Settings", 20, COLOR_TEXT);

    struct {
        const char *title;
        lv_event_cb_t cb;
        lv_obj_t **sub;
        const char *icon;
    } rows[3] = {
        { "WiFi network", on_open_wifi, &s_set_wifi_sub, LV_SYMBOL_WIFI },
        { "Backend server", on_open_backend, &s_set_backend_sub, LV_SYMBOL_DRIVE },
        { "Reset network", on_reset_network, NULL, LV_SYMBOL_REFRESH },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_button_create(s_settings);
        lv_obj_set_pos(row, 12, 72 + i * 88);
        lv_obj_set_size(row, 456, 76);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_event_cb(row, rows[i].cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, rows[i].icon);
        lv_obj_set_style_text_font(ic, ui_font(20), 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, rows[i].title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, 372);
        lv_obj_set_style_text_font(t, ui_font(16), 0);
        lv_obj_set_style_text_color(t, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 52, -12);

        if (rows[i].sub != NULL) {
            lv_obj_t *sub = lv_label_create(row);
            lv_label_set_text(sub, "");
            lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
            lv_obj_set_width(sub, 372);
            lv_obj_set_style_text_font(sub, ui_font(12), 0);
            lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_DIM), 0);
            lv_obj_align(sub, LV_ALIGN_LEFT_MID, 52, 14);
            *rows[i].sub = sub;
        }
    }
}

static lv_obj_t *build_form_common(lv_event_cb_t back_cb, const char *title)
{
    lv_obj_t *scr = new_screen();
    make_back_button(scr, back_cb);
    ui_label(scr, 88, 12, 300, title, 20, COLOR_TEXT);
    return scr;
}

static void build_wifi_form(void)
{
    s_wifi_form = build_form_common(on_back_settings, "WiFi setup");

    ui_label(s_wifi_form, 12, 62, 200, "SSID", 12, COLOR_DIM);
    s_ta_ssid = make_textarea(s_wifi_form, 12, 82, 456, "network name", false);
    ui_label(s_wifi_form, 12, 136, 200, "Password", 12, COLOR_DIM);
    s_ta_pass = make_textarea(s_wifi_form, 12, 156, 456, "network password", true);

    lv_obj_t *save = make_button(s_wifi_form, 12, 212, 456, 48,
                                 "Save & Restart", COLOR_GREEN, 16);
    lv_obj_add_event_cb(save, on_save_wifi, LV_EVENT_CLICKED, NULL);

    s_kb_wifi = lv_keyboard_create(s_wifi_form);
    lv_obj_set_size(s_kb_wifi, SCREEN_W, 210);
    lv_obj_align(s_kb_wifi, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb_wifi, s_ta_ssid);

    lv_obj_add_event_cb(s_ta_ssid, on_ta_focus, LV_EVENT_FOCUSED, s_ta_ssid);
    lv_obj_add_event_cb(s_ta_pass, on_ta_focus, LV_EVENT_FOCUSED, s_ta_pass);
}

static void build_backend_form(void)
{
    s_backend_form = build_form_common(on_back_settings, "Backend setup");

    ui_label(s_backend_form, 12, 62, 300, "Host (IP or hostname)", 12, COLOR_DIM);
    s_ta_host = make_textarea(s_backend_form, 12, 82, 456, "192.168.1.100", false);
    ui_label(s_backend_form, 12, 136, 200, "Port", 12, COLOR_DIM);
    s_ta_port = make_textarea(s_backend_form, 12, 156, 456, "8080", false);
    lv_textarea_set_max_length(s_ta_port, 5);

    lv_obj_t *save = make_button(s_backend_form, 12, 212, 456, 48,
                                 "Save & Restart", COLOR_GREEN, 16);
    lv_obj_add_event_cb(save, on_save_backend, LV_EVENT_CLICKED, NULL);

    s_kb_backend = lv_keyboard_create(s_backend_form);
    lv_obj_set_size(s_kb_backend, SCREEN_W, 210);
    lv_obj_align(s_kb_backend, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb_backend, s_ta_host);

    lv_obj_add_event_cb(s_ta_host, on_ta_focus, LV_EVENT_FOCUSED, s_ta_host);
    lv_obj_add_event_cb(s_ta_port, on_ta_focus, LV_EVENT_FOCUSED, s_ta_port);
}

static void build_provision(void)
{
    s_provision = new_screen();
    ui_label(s_provision, 12, 12, 456, "Setup Mode", 20, COLOR_TEXT);

    s_prov_qr = lv_qrcode_create(s_provision);
    lv_qrcode_set_size(s_prov_qr, 220);
    lv_qrcode_set_dark_color(s_prov_qr, lv_color_black());
    lv_qrcode_set_light_color(s_prov_qr, lv_color_white());
    lv_obj_set_pos(s_prov_qr, 130, 64);

    s_prov_info = ui_label(s_provision, 24, 306, 432, "", 14, COLOR_TEXT);
    lv_obj_set_style_text_align(s_prov_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_prov_info, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = ui_label(s_provision, 24, 396, 432,
        "Scan with your phone to join, the setup page\nopens automatically (or visit http://192.168.4.1)",
        12, COLOR_DIM);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

/* --- public API ----------------------------------------------------------- */

void ui_panel_init(void)
{
    build_home();
    build_detail();
    build_settings();
    build_wifi_form();
    build_backend_form();
    build_provision();
    lv_screen_load(s_home);
    refresh_grid();
}

void ui_show_provisioning(const char *ap_ssid, const char *qr_payload)
{
    s_screen = SCR_PROVISION;
    lv_qrcode_update(s_prov_qr, qr_payload, strlen(qr_payload));
    lv_label_set_text_fmt(s_prov_info,
                          "Join WiFi:  %s\nThen open:  http://192.168.4.1",
                          ap_ssid);
    lv_screen_load(s_provision);
}

void ui_panel_tick(void)
{
    /* consume shared state updates produced by the network task */
    if (g_snap_dirty && xSemaphoreTake(g_snap_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(s_sessions, g_snap.sessions, sizeof(s_sessions));
        s_count = g_snap.count;
        xSemaphoreGive(g_snap_lock);
        g_snap_dirty = false;

        if (s_screen == SCR_HOME) {
            refresh_grid();
        }
    }

    if (g_output_dirty) {
        g_output_dirty = false;
        if (s_screen == SCR_DETAIL) {
            lv_label_set_text(s_det_output, g_output_buf);
        }
    }

    if (g_toast_dirty) {
        g_toast_dirty = false;
        if (s_screen == SCR_DETAIL) {
            lv_label_set_text(s_det_toast, g_toast);
        }
    }

    if (g_conn_dirty) {
        g_conn_dirty = false;
        lv_label_set_text(s_conn_label, g_conn);
    }

    /* keep the detail output fresh while the page is open */
    if (s_screen == SCR_DETAIL && s_selected >= 0 && s_selected < s_count) {
        uint32_t now = lv_tick_get();
        if (now - s_last_output_fetch > 3000) {
            panel_fetch_output(s_sessions[s_selected].pane_id);
            s_last_output_fetch = now;
        }
    }
}
