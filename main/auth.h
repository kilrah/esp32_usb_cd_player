/*
 * Admin password for the web UI's sensitive actions (firmware upload, Wi-Fi settings, reboot, setup AP,
 * password change). HTTP Basic authentication; the user name is ignored. Player controls stay open.
 *
 * The password is stored in NVS (namespace "web", key "admin"), set from the web UI. Without one (the default,
 * or CONFIG_CD_ADMIN_PASSWORD), nothing is protected. The serial command 'P' clears it (physical access =
 * recovery).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define AUTH_PASSWORD_MAX 64

void auth_init(void);

/** A password is set */
bool auth_enabled(void);

/**
 * Check the request's credentials. If they are missing or wrong, a 401 response has already been sent
 * and false is returned: the handler must then just return ESP_OK.
 */
bool auth_check(httpd_req_t *req);

/** Set a new password (empty = no protection) and store it in NVS */
esp_err_t auth_set_password(const char *password);
