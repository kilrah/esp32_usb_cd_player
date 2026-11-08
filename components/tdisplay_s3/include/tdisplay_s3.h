/*
 * LilyGO T-Display S3: ST7789 (i80, 8-bit) + LVGL 9 initialization
 *
 * Based on https://github.com/hiruna/esp-idf-t-display-s3
 * SPDX-FileCopyrightText: © 2025 Hiruna Wijesinghe <hiruna.kawinda@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display pins (fixed on the board) */
#define TDS3_PIN_LCD_PWR      15
#define TDS3_PIN_LCD_BL       38
#define TDS3_PIN_LCD_D0       39
#define TDS3_PIN_LCD_D1       40
#define TDS3_PIN_LCD_D2       41
#define TDS3_PIN_LCD_D3       42
#define TDS3_PIN_LCD_D4       45
#define TDS3_PIN_LCD_D5       46
#define TDS3_PIN_LCD_D6       47
#define TDS3_PIN_LCD_D7       48
#define TDS3_PIN_LCD_WR       8
#define TDS3_PIN_LCD_RD       9
#define TDS3_PIN_LCD_DC       7
#define TDS3_PIN_LCD_CS       6
#define TDS3_PIN_LCD_RST      5

/* Buttons. In landscape orientation (buttons on the left), BTN1 is at the top, BTN2 at the bottom. */
#define TDS3_PIN_BTN1         0   /* BOOT */
#define TDS3_PIN_BTN2         14

#define TDS3_LCD_H_RES        320
#define TDS3_LCD_V_RES        170

/**
 * @brief Power up the LCD, initialize the i80 bus, ST7789 panel and the LVGL port
 *        (landscape, buttons on the left), and switch on the backlight.
 *
 * Use lvgl_port_lock()/lvgl_port_unlock() around LVGL calls from other tasks.
 */
esp_err_t tdisplay_s3_init(lv_display_t **disp);

/** Backlight on (full brightness) / off */
void tdisplay_s3_backlight(bool on);

#define TDS3_BRIGHTNESS_MAX 16

/** Backlight brightness 0 (off) .. TDS3_BRIGHTNESS_MAX (AW9364 driver, 16 steps) */
void tdisplay_s3_set_brightness(int level);
int tdisplay_s3_get_brightness(void);

#ifdef __cplusplus
}
#endif
