#pragma once

/* Shared UI helpers: palette, fonts, label factory. */

#include "lvgl.h"

#define COLOR_BG        0x0f1419
#define COLOR_PANEL     0x161c23
#define COLOR_ELEVATED  0x1c242d
#define COLOR_BORDER    0x27313c
#define COLOR_TEXT      0xd7dee7
#define COLOR_DIM       0x8494a5
#define COLOR_ACCENT    0x4c9aff
#define COLOR_GREEN     0x57ab5a
#define COLOR_RED       0xe5534b

static inline lv_font_t *ui_font(int size)
{
    switch (size) {
    case 12: return (lv_font_t *)&lv_font_montserrat_12;
    case 14: return (lv_font_t *)&lv_font_montserrat_14;
    case 20: return (lv_font_t *)&lv_font_montserrat_20;
    case 24: return (lv_font_t *)&lv_font_montserrat_24;
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
