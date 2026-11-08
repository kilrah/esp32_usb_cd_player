/*
 * Minimal captive-portal DNS server for the setup access point: answers every A query with the AP's
 * address, so phones show the "sign in to network" page (the web UI) after joining.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/** Start the server on UDP port 53 of @p ip (network byte order, e.g. 192.168.4.1). Idempotent. */
esp_err_t captive_dns_start(uint32_t ip);
