/*
 * Web UI: player controls and settings in the browser (http://cdplayer.local/)
 *
 *   GET  /                 the page (main/web/index.html, embedded)
 *   GET  /api/status       player, network, firmware state (JSON)
 *   GET  /api/tracks       track list of the loaded disc (JSON)
 *   GET  /api/cover        cover art of the loaded disc (cached JPEG, 404 if none)
 *   POST /api/cmd?c=..&v=  player command, returns /api/status
 *   POST /api/wifi         form body ssid=..&pass=.. (URL-encoded)
 *   POST /api/reboot
 * Firmware upload uses POST /ota (ota.c).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Number of URI handlers web_register() adds */
#define WEB_URI_HANDLERS 7

esp_err_t web_register(httpd_handle_t server);
