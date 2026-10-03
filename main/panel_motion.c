/*
 * Motion wake source (UI_DESIGN.md §4.3) — QMI8658 accelerometer.
 *
 * The IMU shares I2C0 with the PMIC, the CST9217 touch controller and the
 * ES8311 codec, so it is added as another device on the existing bus rather
 * than given a driver of its own. Polling runs at 10 Hz from a 3 KiB task,
 * which is cheap next to the panel's 200 ms LVGL tick and keeps the C6's
 * radio duty cycle untouched.
 *
 * Wake rule: the vector magnitude must leave 1 g by more than the threshold
 * below *and* stay out for two consecutive samples. Requiring two samples
 * rejects the single-sample spikes that I2C noise produces; the threshold
 * itself is what rejects a device that is merely sitting on a desk.
 */

#include "panel_motion.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The vendor header defines M_PI and ONE_G itself. Defining either first
 * would collide under -Werror, so make sure the header's own definition is
 * the only one the preprocessor sees. */
#ifdef M_PI
#undef M_PI
#endif
#ifdef ONE_G
#undef ONE_G
#endif
#include "qmi8658.h"

static const char *TAG = "panel_motion";

/* The driver's ODR ladder has no 100 Hz step, so ask for 125 Hz and poll at
 * 10 Hz: the extra sensor rate is what lets two consecutive samples reject a
 * single-sample spike, and the sensor costs microamps either way. */
#define SAMPLE_PERIOD_MS   100
#define SENSOR_ODR         QMI8658_ACCEL_ODR_125HZ
#define WAKE_DELTA         3.0f   /* m/s^2 away from 1 g */
#define WAKE_STREAK        2      /* consecutive out-of-box samples */
#define TASK_STACK         3072

static qmi8658_dev_t s_dev;
static bool s_present;
static volatile bool s_wake_pending;
static volatile float s_magnitude = -1.0f;

static void motion_task(void *arg)
{
    (void)arg;
    int streak = 0;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        qmi8658_data_t d;
        bool ready = false;
        if (qmi8658_is_data_ready(&s_dev, &ready) != ESP_OK || !ready) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }
        if (qmi8658_read_sensor_data(&s_dev, &d) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }

        float mag = sqrtf(d.accelX * d.accelX +
                          d.accelY * d.accelY +
                          d.accelZ * d.accelZ);
        s_magnitude = mag;

        if (fabsf(mag - 9.807f) > WAKE_DELTA) {
            if (++streak >= WAKE_STREAK) {
                streak = 0;
                s_wake_pending = true;
                last_wake = xTaskGetTickCount();
            }
        } else {
            streak = 0;
        }

        /* A long quiet spell means the device has not been touched; drop the
         * task to 2 Hz so a panel left alone all night does not keep polling
         * the bus at the rate it uses while someone is watching it. */
        TickType_t idle = xTaskGetTickCount() - last_wake;
        vTaskDelay(pdMS_TO_TICKS(idle > pdMS_TO_TICKS(30000)
                                      ? 500 : SAMPLE_PERIOD_MS));
    }
}

bool panel_motion_start(i2c_master_bus_handle_t bus)
{
    if (s_present) return true;

    esp_err_t err = qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_HIGH);
    if (err != ESP_OK) {
        /* Try the other address: on some board revisions the IMU straps low.
         * Absence is not fatal — touch still wakes the panel. */
        err = qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_LOW);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IMU at 0x6A/0x6B (%s); motion wake disabled",
                 esp_err_to_name(err));
        return true;   /* handled, just without motion */
    }

    qmi8658_set_accel_unit_mps2(&s_dev, true);
    if (qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_4G) != ESP_OK ||
        qmi8658_set_accel_odr(&s_dev, SENSOR_ODR) != ESP_OK ||
        qmi8658_enable_accel(&s_dev, true) != ESP_OK) {
        ESP_LOGW(TAG, "IMU found but could not be configured; motion wake off");
        return true;
    }

    s_present = true;
    if (xTaskCreate(motion_task, "panel_motion", TASK_STACK, NULL, 4, NULL)
            != pdPASS) {
        ESP_LOGW(TAG, "motion task could not start; motion wake off");
        s_present = false;
        return false;
    }
    ESP_LOGI(TAG, "IMU ready; wake on |a| deviating from 1 g by %.1f m/s^2",
             WAKE_DELTA);
    return true;
}

bool panel_motion_wake_pending(void)
{
    if (!s_present || !s_wake_pending) return false;
    s_wake_pending = false;
    return true;
}

bool panel_motion_present(void)
{
    return s_present;
}

float panel_motion_accel_magnitude(void)
{
    return s_present ? s_magnitude : -1.0f;
}