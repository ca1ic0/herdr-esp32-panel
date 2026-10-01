#ifndef USER_CONFIG_H
#define USER_CONFIG_H

/*
 * Waveshare ESP32-C6-Touch-AMOLED-2.16 board pin map.
 *
 * Reference: https://www.waveshare.net/wiki/ESP32-C6-Touch-AMOLED-2.16
 * Verified against both the ESP-IDF and Arduino vendor examples
 * (DisplayPort constructor defaults match this table).
 */

#define BSP_I2C_NUM             (I2C_NUM_0)
#define BSP_LCD_SPI_NUM         (SPI2_HOST)

#define BSP_I2C_SCL             (GPIO_NUM_7)
#define BSP_I2C_SDA             (GPIO_NUM_8)

#define BSP_LCD_H_RES           (480)
#define BSP_LCD_V_RES           (480)

/* QSPI (SH8601 AMOLED driver) */
#define BSP_LCD_PCLK            (GPIO_NUM_0)
#define BSP_LCD_DATA0           (GPIO_NUM_1)
#define BSP_LCD_DATA1           (GPIO_NUM_2)
#define BSP_LCD_DATA2           (GPIO_NUM_3)
#define BSP_LCD_DATA3           (GPIO_NUM_4)
#define BSP_LCD_CS              (GPIO_NUM_15)
#define BSP_LCD_BITS_PER_PIXEL  (16)

/* Backlight is driven through the SH8601 DCS register, no GPIO. */
#define BSP_LCD_BACKLIGHT       (GPIO_NUM_NC)
#define BSP_LCD_RST             (GPIO_NUM_NC)

/* Capacitive touch (CST9217 over I2C) */
#define BSP_LCD_TOUCH_RST       (GPIO_NUM_11)
#define BSP_LCD_TOUCH_INT       (GPIO_NUM_5)

/* ES8311 I2S TX: Waveshare 07_Audio_Test example pin map. Confirm on the
 * assembled board before claiming audible output; speaker uses 2PIN pad. */
#define BSP_I2S_SCLK            (GPIO_NUM_20)
#define BSP_I2S_MCLK            (GPIO_NUM_19)
#define BSP_I2S_LCLK            (GPIO_NUM_22)
#define BSP_I2S_DOUT            (GPIO_NUM_23)

#endif /* USER_CONFIG_H */
