#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool battery_present;
    bool external_power;
    bool charging;
    int percent;       /* -1 means unavailable */
    int millivolts;    /* 0 means unavailable */
} panel_power_status_t;

/* Returns false until the first background PMIC sample succeeds. */
bool panel_power_get(panel_power_status_t *out);

#ifdef __cplusplus
}
#endif
