/*
 * Wi-Fi station + mDNS; starts the OTA/info HTTP server (ota.c) once an IP address is obtained
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool configured;   /*!< An SSID is set (NVS, or the Kconfig default) */
    bool connected;    /*!< Associated and has an IP address */
    char ssid[33];
    char ip[16];
} net_status_t;

/** Start Wi-Fi (requires nvs_flash_init()). Connects in the background and reconnects with backoff. */
esp_err_t net_init(void);

/** Store new credentials in NVS and reconnect. An empty SSID clears them and disconnects. */
esp_err_t net_set_credentials(const char *ssid, const char *password);

void net_get_status(net_status_t *st);

/** Associated and has an IP address */
bool net_is_connected(void);

/** Print network, firmware and storage information to the console */
void net_print_info(void);
