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
 * Latin/digits use Montserrat; CJK glyphs resolve through the LVGL 9.5
 * `fallback` chain to a generated Noto Sans CJK subset (GB2312 level-1,
 * tools/fonts/make_cjk_fonts.sh). When main/fonts/cjk_fonts.h does not
 * exist the firmware still builds, but CJK renders as '□'.
 */
#if defined(__has_include)
#if __has_include("fonts/cjk_fonts.h")
#include "fonts/cjk_fonts.h"
#endif
#endif

static inline lv_font_t *ui_font(int size)
{
#ifdef UI_HAVE_CJK_FONT
    /* Mutable copies of the built-in Montserrat fonts with the CJK subset
     * chained as fallback. The built-ins are const, so copy then chain.
     * Every size must chain a CJK fallback: Montserrat has no CJK glyphs, so
     * without it Chinese text renders as the .notdef box. There is no
     * generated 12/14 px CJK cut, so those chain the 16 px cut (nearest). */
    static lv_font_t f12, f14, f16, f20, f24;
    static bool ready;
    if (!ready) {
        f12 = lv_font_montserrat_12;
        f12.fallback = &font_cjk_16;
        f14 = lv_font_montserrat_14;
        f14.fallback = &font_cjk_16;
        f16 = lv_font_montserrat_16;
        f16.fallback = &font_cjk_16;
        f20 = lv_font_montserrat_20;
        f20.fallback = &font_cjk_20;
        f24 = lv_font_montserrat_24;
        f24.fallback = &font_cjk_24;
        ready = true;
    }
    switch (size) {
    case 12: return &f12;
    case 14: return &f14;
    case 18:
    case 20: return &f20;
    case 24:
    case 26:
    case 28: return &f24;
    default: return &f16;
    }
#else
    switch (size) {
    case 12: return (lv_font_t *)&lv_font_montserrat_12;
    case 14: return (lv_font_t *)&lv_font_montserrat_14;
    case 20: return (lv_font_t *)&lv_font_montserrat_20;
    case 24: return (lv_font_t *)&lv_font_montserrat_24;
    case 28: return (lv_font_t *)&lv_font_montserrat_24;  /* nearest available */
    case 18: return (lv_font_t *)&lv_font_montserrat_20;  /* nearest available */
    default: return (lv_font_t *)&lv_font_montserrat_16;
    }
#endif
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
