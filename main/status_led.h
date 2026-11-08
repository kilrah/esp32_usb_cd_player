/*
 * Status indication on the on-board WS2812B LED
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_BOOT,          /*!< White flash: firmware just started */
    STATUS_WAIT_DRIVE,    /*!< Blue slow blink: waiting for USB drive */
    STATUS_NO_DISC,       /*!< Yellow steady: drive connected, no disc */
    STATUS_DISC_LOADING,  /*!< Yellow fast blink: disc spinning up / tray moving */
    STATUS_BUSY,          /*!< Cyan steady: reading TOC / audio */
    STATUS_DISC_READY,    /*!< Green steady: disc ready */
    STATUS_ERROR,         /*!< Red fast blink: error */
} status_t;

esp_err_t status_led_init(int gpio);
void status_led_set(status_t status);
