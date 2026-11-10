/*
 * Board interface used by the application (every board component provides this header)
 *
 * Guition JC1060P470 (ESP32-P4): 7" 1024x600 MIPI-DSI (JD9165) with GT911 touch, PWM backlight,
 * audio: external PCM5102 on the GPIO1-4 header (default) or the on-board ES8311 + NS4150 speaker amplifier
 * (mono), ESP32-C6 for Wi-Fi (ESP-Hosted over SDIO).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_NAME            "Guition JC1060P470"
#define BOARD_LCD_H_RES       1024
#define BOARD_LCD_V_RES       600
#define BOARD_HAS_TOUCH       1
#define BOARD_BRIGHTNESS_MAX  16  /* same scale as the T-Display S3; mapped to PWM duty */

/* Display / touch / backlight (fixed on the board) */
#define BOARD_PIN_LCD_RST     5
#define BOARD_PIN_LCD_BL      23
#define BOARD_PIN_I2C_SDA     7   /* GT911 touch, ES8311 codec */
#define BOARD_PIN_I2C_SCL     8

#define BOARD_PIN_PA_EN       11  /* NS4150 speaker amplifier enable */

#if CONFIG_JC1060_AUDIO_ES8311
/* On-board ES8311 codec (I2S slave, MCLK = 256 fs) and NS4150 speaker amplifier */
#define BOARD_I2S_MCLK        13
#define BOARD_I2S_BCLK        12
#define BOARD_I2S_WS          10
#define BOARD_I2S_DOUT        9
#define BOARD_AUDIO_MONO      1   /* ES8311 has one DAC: the output mixes left and right */
#else
/* External PCM5102 module plugged 1:1 onto the VCC/GND/GPIO1-4 header (menuconfig: Board > Audio output) */
#define BOARD_I2S_WS          1   /* PCM5102 LRCK */
#define BOARD_I2S_DOUT        2   /* PCM5102 DIN */
#define BOARD_I2S_BCLK        3   /* PCM5102 BCK */
#define BOARD_I2S_MCLK        4   /* PCM5102 SCK */
#define BOARD_AUDIO_MONO      0
#endif

/** Display, LVGL port and touch. Use lvgl_port_lock()/unlock() around LVGL calls. */
esp_err_t board_display_init(lv_display_t **disp);

/** Backlight 0 (off) .. BOARD_BRIGHTNESS_MAX */
void board_set_brightness(int level);
int board_get_brightness(void);

/** Called once the I2S clocks run: configures the ES8311 and switches the speaker amplifier on
 *  (PCM5102 output: keeps the amplifier off) */
esp_err_t board_audio_init(void);
