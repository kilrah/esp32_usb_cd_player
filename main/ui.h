/*
 * CD player screen (LVGL, 320x170 landscape)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_err.h"

typedef enum {
    UI_DIM_NEVER,   /*!< Always at the "on" level (except "off after") */
    UI_DIM_IDLE,    /*!< Dim only while nothing is playing (default) */
    UI_DIM_ALWAYS,  /*!< Dim after the delay, also during playback */
} ui_dim_mode_t;

/** Backlight settings (persisted in NVS, namespace "display") */
typedef struct {
    ui_dim_mode_t mode;
    int on_level;       /*!< 1..16 */
    int dim_level;      /*!< 0..16 */
    int dim_delay_s;    /*!< without activity, >= 5 */
    int off_delay_s;    /*!< idle (nothing playing) and no activity; 0 = never */
} ui_backlight_cfg_t;

/** Initialize the display and build the player screen. Refreshes itself from player_get_status(). */
esp_err_t ui_init(void);

void ui_get_backlight(ui_backlight_cfg_t *cfg);

/** Validate (clamps the values), apply and store. Counts as activity, so the "on" level shows right away. */
esp_err_t ui_set_backlight(const ui_backlight_cfg_t *cfg);

const char *ui_dim_mode_name(ui_dim_mode_t mode);
