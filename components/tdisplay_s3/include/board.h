/*
 * Board interface used by the application (every board component provides this header)
 *
 * LilyGO T-Display S3: 320x170 ST7789, no touch, AW9364 backlight (16 levels), external PCM5102 DAC.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"
#include "tdisplay_s3.h"

#define BOARD_NAME            "LilyGO T-Display S3"
#define BOARD_LCD_H_RES       TDS3_LCD_H_RES
#define BOARD_LCD_V_RES       TDS3_LCD_V_RES
#define BOARD_HAS_DISPLAY     1
#define BOARD_HAS_TOUCH       0
#define BOARD_BRIGHTNESS_MAX  TDS3_BRIGHTNESS_MAX

/* PCM5102 wiring (I2S) */
#define BOARD_I2S_MCLK        17  /* PCM5102 SCK */
#define BOARD_I2S_BCLK        18  /* PCM5102 BCK */
#define BOARD_I2S_WS          43  /* PCM5102 LCK */
#define BOARD_I2S_DOUT        44  /* PCM5102 DIN */
#define BOARD_AUDIO_MONO      0   /* the output is stereo */

/** Display and LVGL port (and touch, if any). Use lvgl_port_lock()/unlock() around LVGL calls. */
static inline esp_err_t board_display_init(lv_display_t **disp)
{
    return tdisplay_s3_init(disp);
}

/** Backlight 0 (off) .. BOARD_BRIGHTNESS_MAX */
static inline void board_set_brightness(int level)
{
    tdisplay_s3_set_brightness(level);
}

static inline int board_get_brightness(void)
{
    return tdisplay_s3_get_brightness();
}

/** Called once the I2S clocks run: set up a codec / power amplifier. The PCM5102 needs nothing. */
static inline esp_err_t board_audio_init(void)
{
    return ESP_OK;
}
