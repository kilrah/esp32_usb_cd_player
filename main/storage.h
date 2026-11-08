/*
 * LittleFS on the "storage" partition, mounted at /data. Meant for cached metadata
 * (track lists, cover art) fetched from the internet.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#define STORAGE_BASE_PATH "/data"

/** Mount the filesystem; formats the partition if it holds no valid LittleFS (first boot) */
esp_err_t storage_init(void);

/** Filesystem size and usage in bytes (0 if not mounted). Cached: call storage_changed() after writing files. */
void storage_get_info(size_t *total, size_t *used);

/** Files were written or deleted: storage_get_info() re-reads the usage */
void storage_changed(void);

void storage_print_info(void);
