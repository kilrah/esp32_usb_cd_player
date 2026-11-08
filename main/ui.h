/*
 * CD player screen (LVGL, 320x170 landscape)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_err.h"

/** Initialize the display and build the player screen. Refreshes itself from player_get_status(). */
esp_err_t ui_init(void);
