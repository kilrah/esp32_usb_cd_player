/*
 * Firmware update over HTTP:
 *   POST /ota   raw application image (build/usbhost_cd.bin) -> written to the other OTA slot, then reboot
 *   GET  /info  JSON with firmware version, ELF SHA-256, running partition, uptime
 * See tools/ota.py. The web UI's handlers are registered on the same server (web.c).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_err.h"

/** Start the HTTP server (idempotent) */
esp_err_t ota_server_start(void);

/**
 * Confirm the running firmware if it was just installed by OTA and is pending verification.
 * Without confirmation the bootloader rolls back to the previous firmware on the next reset.
 */
void ota_confirm_running_app(const char *reason);

/** Upload progress 0..100 while an update is being received, -1 otherwise */
int ota_progress(void);

/** OTA state of the running firmware: undefined (cable flash), pending-verify, valid, ... */
const char *ota_running_state_name(void);

void ota_log_boot_info(void);
void ota_print_info(void);
