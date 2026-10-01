#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_AUDIO_REQUEST = 1,
    PANEL_AUDIO_DONE = 2,
} panel_audio_kind_t;

/* One task owns ES8311 and I2S. Non-blocking calls from UI/worker only. */
bool panel_audio_start(i2c_master_bus_handle_t bus);
bool panel_audio_ready(void);
bool panel_audio_play(panel_audio_kind_t kind);
void panel_audio_stop(void);
void panel_audio_set_volume(uint8_t percent);

#ifdef __cplusplus
}
#endif
