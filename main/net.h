/*
 * Wi-Fi station + setup access point + mDNS; starts the OTA/web HTTP server (ota.c)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define NET_AP_IP_STR "192.168.4.1"

typedef struct {
    bool configured;   /*!< An SSID is set (NVS, or the Kconfig default) */
    bool connected;    /*!< Associated and has an IP address */
    char hostname[33]; /*!< DHCP / mDNS name (<hostname>.local) */
    char ssid[33];
    char ip[16];
    bool ap_active;    /*!< Setup access point is on */
    char ap_ssid[33];
    int ap_clients;
} net_status_t;

/** Start Wi-Fi (requires nvs_flash_init()). Connects in the background and reconnects with backoff. */
esp_err_t net_init(void);

#define NET_HOSTNAME_MAX 32

/**
 * Set the DHCP / mDNS hostname (stored in NVS, namespace "net"; "" = back to CONFIG_CD_HOSTNAME).
 * Letters, digits and '-', not at the start or end; stored in lower case. mDNS uses it right away,
 * the DHCP server sees it at the next lease.
 * @return ESP_ERR_INVALID_ARG if the name isn't valid
 */
esp_err_t net_set_hostname(const char *name);

/** Store new credentials in NVS and reconnect. An empty SSID clears them and disconnects. */
esp_err_t net_set_credentials(const char *ssid, const char *password);

void net_get_status(net_status_t *st);

/** Associated and has an IP address */
bool net_is_connected(void);

/** Setup AP password ("" = open network) */
const char *net_ap_password(void);

/**
 * Switch the setup AP on or off by request (web UI), e.g. to move the player to another network.
 * Switched on this way, it stays on until switched off or the next reboot.
 */
void net_setup_ap(bool on);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} net_scan_entry_t;

/** Scan for networks (blocking, ~2 s). Strongest first, duplicates merged. @return number of entries, <0 on error */
int net_scan(net_scan_entry_t *out, int max);

/** Print network, firmware and storage information to the console */
void net_print_info(void);
