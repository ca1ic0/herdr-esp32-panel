#pragma once

/*
 * Motion wake source (UI_DESIGN.md §4.3).
 *
 * The board carries a QMI8658 6-axis IMU on the shared I2C0 bus. The panel has
 * no physical buttons, so "pick the panel up" is the only hands-free way back
 * from the standby screen. Touch already resets LVGL's inactivity timer, so
 * only motion needs handling here.
 *
 * The driver runs in its own low-rate task and publishes a single sticky
 * "something moved" flag that the LVGL tick consumes. Sampling continues in
 * standby (the sensor is cheap and the panel is still powered) but the flag
 * is only raised when the acceleration leaves a small box around 1 g, which
 * rejects desk vibration and the slow drift of a device left alone.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the motion-wake task. Safe to call when no IMU is fitted: the task
 * then parks itself and panel_motion_wake_pending() stays false forever.
 * Returns false only when the task could not be created.
 */
bool panel_motion_start(i2c_master_bus_handle_t bus);

/** True once per detected motion. Reading clears the flag. */
bool panel_motion_wake_pending(void);

/** True when an IMU was found. Lets settings say "No IMU" instead of lying. */
bool panel_motion_present(void);

/** Current acceleration magnitude in m/s^2, or -1 when the IMU is absent. */
float panel_motion_accel_magnitude(void);

#ifdef __cplusplus
}
#endif