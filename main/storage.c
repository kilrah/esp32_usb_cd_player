/*
 * LittleFS on the "storage" partition, mounted at /data
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdbool.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "storage";

#define PARTITION_LABEL "storage"

static bool s_mounted;

esp_err_t storage_init(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mounting LittleFS on \"%s\" failed: %s", PARTITION_LABEL, esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    esp_littlefs_info(PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s: %u KiB used of %u KiB", STORAGE_BASE_PATH,
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

void storage_get_info(size_t *total, size_t *used)
{
    *total = *used = 0;
    if (s_mounted) {
        esp_littlefs_info(PARTITION_LABEL, total, used);
    }
}

void storage_print_info(void)
{
    size_t total = 0, used = 0;
    if (s_mounted && esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        printf("STORAGE %s used=%u KiB total=%u KiB\n", STORAGE_BASE_PATH, (unsigned)(used / 1024),
               (unsigned)(total / 1024));
    } else {
        printf("STORAGE not mounted\n");
    }
}
