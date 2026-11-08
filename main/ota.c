/*
 * Firmware update over HTTP (push): the image is streamed into the inactive OTA slot.
 *
 * The new firmware boots in "pending verify" state (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). It is
 * confirmed once it has connected to Wi-Fi and started this server again, i.e. once another OTA
 * update is possible. If it crashes or can't get there, the next reset boots the previous firmware.
 *
 * No authentication: anyone on the LAN can flash the device. Fine for a home network prototype.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "player.h"
#include "ota.h"
#include "web.h"

static const char *TAG = "ota";

#define RECV_BUF_SIZE  4096
/* Image header + first segment header + app descriptor: enough to check the project name */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEADER_LEN      (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

static httpd_handle_t s_server;
static volatile int s_progress = -1;

int ota_progress(void)
{
    return s_progress;
}

static const char *ota_state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined"; /* flashed by cable */
    }
}

static esp_ota_img_states_t running_state(void)
{
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
    return state;
}

const char *ota_running_state_name(void)
{
    return ota_state_name(running_state());
}

void ota_confirm_running_app(const char *reason)
{
    if (running_state() == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "New firmware confirmed (%s), rollback cancelled", reason);
        } else {
            ESP_LOGE(TAG, "Confirming firmware failed: %s", esp_err_to_name(err));
        }
    }
}

void ota_log_boot_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state = running_state();
    ESP_LOGI(TAG, "Firmware %s (%s %s), partition %s, state %s", app->version, app->date, app->time,
             run->label, ota_state_name(state));
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Firmware not yet confirmed: it will be once Wi-Fi is up, otherwise the next reset rolls back");
    }
}

void ota_print_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    printf("FIRMWARE version=%s built=\"%s %s\" elf=%s partition=%s state=%s\n", app->version, app->date,
           app->time, sha, esp_ota_get_running_partition()->label, ota_state_name(running_state()));
}

/* ------------------------------------------------------------------------ */

static esp_err_t send_status(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    /* Full hash from the descriptor: esp_app_get_elf_sha256() is limited to CONFIG_APP_RETRIEVE_LEN_ELF_SHA (9) chars */
    char sha[65];
    for (int i = 0; i < 32; i++) {
        snprintf(sha + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    }
    char json[512];
    snprintf(json, sizeof(json),
             "{\"project\":\"%s\",\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"idf\":\"%s\","
             "\"elf_sha256\":\"%s\",\"partition\":\"%s\",\"ota_state\":\"%s\",\"uptime_s\":%"PRIu64","
             "\"free_heap\":%"PRIu32"}\n",
             app->project_name, app->version, app->date, app->time, app->idf_ver, sha,
             esp_ota_get_running_partition()->label, ota_state_name(running_state()),
             (uint64_t)(esp_timer_get_time() / 1000000), esp_get_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/** Reject images that aren't app images for this project (e.g. a bootloader or another project's .bin) */
static const char *check_image_header(const uint8_t *buf)
{
    const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + APP_DESC_OFFSET);
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC) {
        return "Not an ESP app image";
    }
    if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "Image is for a different chip";
    }
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "Image has no app descriptor";
    }
    if (strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name)) != 0) {
        return "Image is for a different project";
    }
    return NULL;
}

static void pause_playback(void)
{
    player_status_t st;
    player_get_status(&st);
    if (st.state == PLAYER_PLAYING) {
        player_command(PLAYER_CMD_PAUSE, 0); /* flash writes stall tasks running from flash: avoid glitches */
        /* Let the I2S DMA ring (~93 ms) play out and get auto-cleared to silence before the flash erase
         * starts. Otherwise its last contents can loop during the erase (audible stutter). */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (s_progress >= 0) {
        return send_status(req, "409 Conflict", "Update already in progress\n");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_status(req, "500 Internal Server Error", "No OTA partition\n");
    }
    const size_t total = req->content_len;
    if (total < HEADER_LEN) {
        return send_status(req, "411 Length Required", "Send the image as the request body with Content-Length\n");
    }
    if (total > part->size) {
        return send_status(req, "413 Payload Too Large", "Image larger than the OTA partition\n");
    }

    uint8_t *buf = malloc(RECV_BUF_SIZE);
    if (!buf) {
        return send_status(req, "500 Internal Server Error", "Out of memory\n");
    }
    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned)total, part->label);
    s_progress = 0;
    pause_playback();

    const char *error = NULL;
    esp_ota_handle_t handle = 0;
    bool begun = false;
    size_t received = 0, fill = 0;
    int timeouts = 0, last_logged = -1;
    int64_t t0 = esp_timer_get_time();

    while (received < total && !error) {
        int n = httpd_req_recv(req, (char *)buf + fill, MIN(RECV_BUF_SIZE - fill, total - received));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) {
            continue;
        }
        if (n <= 0) {
            error = "Connection lost";
            break;
        }
        timeouts = 0;
        received += n;
        fill += n;
        if (!begun) {
            if (fill < HEADER_LEN) {
                continue;
            }
            error = check_image_header(buf);
            if (error) {
                break;
            }
            /* Erase exactly the image size up front: uses 64 KB block erases, much faster than
             * OTA_WITH_SEQUENTIAL_WRITES (one 4 KB sector erase per write, ~60 KiB/s overall) */
            esp_err_t err = esp_ota_begin(part, total, &handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
                error = "esp_ota_begin failed";
                break;
            }
            begun = true;
        }
        if (esp_ota_write(handle, buf, fill) != ESP_OK) {
            error = "Flash write failed";
            break;
        }
        fill = 0;
        s_progress = (int)((uint64_t)received * 100 / total);
        if (s_progress / 10 != last_logged) {
            last_logged = s_progress / 10;
            ESP_LOGI(TAG, "%d %%", s_progress);
        }
    }
    free(buf);

    if (!error && begun) {
        esp_err_t err = esp_ota_end(handle);
        begun = false;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            error = "Image validation failed (corrupt upload?)";
        } else if (err != ESP_OK) {
            error = "esp_ota_end failed";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            error = "Setting the boot partition failed";
        }
    }
    if (begun) {
        esp_ota_abort(handle);
    }
    if (error) {
        ESP_LOGE(TAG, "Update failed after %u bytes: %s", (unsigned)received, error);
        s_progress = -1;
        char msg[96];
        snprintf(msg, sizeof(msg), "Update failed: %s\n", error);
        return send_status(req, "400 Bad Request", msg);
    }

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "Update written to %s in %"PRId64" ms (%"PRId64" KiB/s), rebooting", part->label, ms,
             ms ? (int64_t)total * 1000 / 1024 / ms : 0);
    send_status(req, "200 OK", "Update OK, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(500)); /* let the response go out */
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 2 + WEB_URI_HANDLERS;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    const httpd_uri_t uris[] = {
        {.uri = "/info", .method = HTTP_GET, .handler = info_get_handler},
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    web_register(s_server);
    ESP_LOGI(TAG, "HTTP server on port %d: web UI at /, GET /info, POST /ota", cfg.server_port);
    return ESP_OK;
}
