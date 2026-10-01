#pragma once

/* Shared UI helpers: palette (UI_DESIGN.md §2), fonts, label factory. */

#include "lvgl.h"

/* Background / surfaces */
#define COLOR_BG        0x080D12
#define COLOR_CARD      0x151E27
#define COLOR_BORDER    0x293643
#define COLOR_PANEL     COLOR_CARD

/* Text */
#define COLOR_TEXT      0xF2F6FA
#define COLOR_DIM       0xAEBCC9
#define COLOR_DISABLED  0x667687

/* Status (never color-only — always pair with shape + text) */
#define COLOR_PENDING   0xFF6B68
#define COLOR_WORKING   0xF7BD4A
#define COLOR_DONE      0x69B5FF
#define COLOR_IDLE      0x70C995
#define COLOR_UNKNOWN   0x8D9BAA

#define COLOR_ACCENT    0x69B5FF

/*
 * Font policy (UI_DESIGN.md §2):
 * Chinese body text needs a CJK-capable face at 18/20/24/28.
 * Montserrat has no CJK glyphs — ui_font() falls back to it for digits/
 * Latin only. Before shipping Chinese UI, register a CJK font and point
 * UI_FONT_CJK_* at it. Missing glyphs render as '□'.
 *
 * If LV_FONT_CJK_20 is defined in sdkconfig/lv_conf it is used.
 */
static inline lv_font_t *ui_font(int size)
{
#if defined(LV_FONT_CJK_28)
    if (size >= 28) return (lv_font_t *)&LV_FONT_CJK_28;
#endif
#if defined(LV_FONT_CJK_24)
    if (size >= 24) return (lv_font_t *)&LV_FONT_CJK_24;
#endif
#if defined(LV_FONT_CJK_20)
    if (size >= 20) return (lv_font_t *)&LV_FONT_CJK_20;
#endif
    switch (size) {
    case 12: return (lv_font_t *)&lv_font_montserrat_12;
    case 14: return (lv_font_t *)&lv_font_montserrat_14;
    case 20: return (lv_font_t *)&lv_font_montserrat_20;
    case 24: return (lv_font_t *)&lv_font_montserrat_24;
    case 28: return (lv_font_t *)&lv_font_montserrat_24;  /* nearest available */
    case 18: return (lv_font_t *)&lv_font_montserrat_20;  /* nearest available */
    default: return (lv_font_t *)&lv_font_montserrat_16;
    }
}

static inline lv_obj_t *ui_label(lv_obj_t *parent, int x, int y, int w,
                                 const char *txt, int size, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_width(lbl, w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, ui_font(size), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    return lbl;
}

static inline lv_obj_t *ui_button(lv_obj_t *parent, int x, int y, int w, int h,
                                  const char *txt, uint32_t bg, int font_size)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, ui_font(font_size), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    return btn;
}
