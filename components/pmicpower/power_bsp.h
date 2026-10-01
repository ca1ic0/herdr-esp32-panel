#pragma once

#include "i2c_bsp.h"

void Custom_PmicPortInit(I2cMasterBus *i2cbus,uint8_t dev_addr);
void Custom_PmicRegisterInit(void);
void Axp2101_isChargingTask(void *arg);

void Axp2101_SetAldo2(uint8_t vol);
void Axp2101_SetAldo3(uint8_t vol);

struct PmicBatteryStatus {
    bool battery_present;
    bool external_power;
    bool charging;
    int percent;       // -1 when unavailable
    int millivolts;    // 0 when unavailable
};

// Call from a background task; I2C reads may block and must not run in LVGL.
bool Custom_PmicReadBattery(PmicBatteryStatus *out);

