#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called on the LVGL task for preview, or during board startup. */
void panel_display_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
