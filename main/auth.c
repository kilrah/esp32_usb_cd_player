/*
 * Admin password (HTTP Basic authentication), see auth.h
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "auth.h"

static const char *TAG = "auth";

#define NVS_NAMESPACE "web"
#define NVS_KEY       "admin"

static char s_password[AUTH_PASSWORD_MAX + 1];

void auth_init(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_password);
    bool found = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        found = nvs_get_str(h, NVS_KEY, s_password, &len) == ESP_OK;
        nvs_close(h);
    }
    if (!found) {
        strlcpy(s_password, CONFIG_CD_ADMIN_PASSWORD, sizeof(s_password));
    }
    ESP_LOGI(TAG, "Admin password %s", s_password[0] ? "set: firmware upload and settings are protected" : "not set");
}

bool auth_enabled(void)
{
    return s_password[0] != '\0';
}

/** Compare without an early exit, so the response time doesn't reveal how much matched */
static bool equal_ct(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = la != lb;
    for (size_t i = 0; i < la; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
    }
    return diff == 0;
}

bool auth_check(httpd_req_t *req)
{
    if (!auth_enabled()) {
        return true;
    }
    char hdr[160];
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
            strncasecmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[128];
        size_t len = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &len, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[len] = '\0';
            const char *colon = strchr((const char *)dec, ':'); /* "user:password", the user name is ignored */
            ok = colon && equal_ct(colon + 1, s_password);
        }
        if (!ok) {
            ESP_LOGW(TAG, "Wrong admin password for %s", req->uri);
        }
    }
    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"CD player admin\", charset=\"UTF-8\"");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Admin password required\n");
    }
    return ok;
}

esp_err_t auth_set_password(const char *password)
{
    if (strlen(password) > AUTH_PASSWORD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        /* Stored even when empty, so an empty password overrides CONFIG_CD_ADMIN_PASSWORD */
        err = nvs_set_str(h, NVS_KEY, password);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        strlcpy(s_password, password, sizeof(s_password));
        ESP_LOGI(TAG, "Admin password %s", password[0] ? "changed" : "removed");
    }
    return err;
}
