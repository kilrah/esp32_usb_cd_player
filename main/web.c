/*
 * Web UI: player controls and settings in the browser. No authentication (home LAN prototype, like OTA).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "player.h"
#include "net.h"
#include "ota.h"
#include "storage.h"
#include "app.h"
#include "auth.h"
#include "ui.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "meta.h"
#include "web.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ------------------------------------------------------------------------ */
/* Chunked JSON writer                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    httpd_req_t *req;
    size_t len;
    char buf[1024];
} jw_t;

static void jw_flush(jw_t *w)
{
    if (w->len) {
        httpd_resp_send_chunk(w->req, w->buf, w->len);
        w->len = 0;
    }
}

static void jw_raw(jw_t *w, const char *s, size_t n)
{
    while (n) {
        if (w->len == sizeof(w->buf)) {
            jw_flush(w);
        }
        size_t k = MIN(n, sizeof(w->buf) - w->len);
        memcpy(w->buf + w->len, s, k);
        w->len += k;
        s += k;
        n -= k;
    }
}

static void jw_printf(jw_t *w, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    jw_raw(w, tmp, MIN((size_t)MAX(n, 0), sizeof(tmp) - 1));
}

/** JSON string literal; UTF-8 passes through, control characters are escaped */
static void jw_str(jw_t *w, const char *s)
{
    jw_raw(w, "\"", 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            jw_raw(w, e, 2);
        } else if (c < 0x20) {
            jw_printf(w, "\\u%04x", c);
        } else {
            jw_raw(w, (const char *)&c, 1);
        }
    }
    jw_raw(w, "\"", 1);
}

/** "key":"value" with a leading comma unless first */
static void jw_kv_str(jw_t *w, const char *key, const char *val)
{
    jw_printf(w, ",\"%s\":", key);
    jw_str(w, val);
}

static void jw_begin(jw_t *w, httpd_req_t *req)
{
    w->req = req;
    w->len = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t jw_end(jw_t *w)
{
    jw_flush(w);
    return httpd_resp_send_chunk(w->req, NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Handlers                                                                 */
/* ------------------------------------------------------------------------ */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static const char *state_name(player_state_t s)
{
    static const char *names[] = {"nodisc", "stopped", "playing", "paused"};
    return names[s];
}

static esp_err_t send_status(httpd_req_t *req)
{
    player_status_t st;
    player_get_status(&st);
    net_status_t net;
    net_get_status(&net);
    jw_t *w = malloc(sizeof(*w));
    if (!w) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    jw_begin(w, req);

    const uint32_t fps = ATAPI_CD_FRAMES_PER_SECOND;
    jw_printf(w, "{\"state\":\"%s\",\"track\":%d,\"tracks\":%d,\"pos\":%"PRIu32",\"len\":%"PRIu32
              ",\"disc_len\":%"PRIu32",\"volume\":%d,\"buffer\":%d,\"buffer_ms\":%"PRIu32
              ",\"underruns\":%"PRIu32",\"read_errors\":%"PRIu32",\"autoplay\":%s,\"ota\":%d,\"preemphasis\":%s,\"deemphasis\":%s",
              state_name(st.state), st.track, st.num_tracks, st.track_pos / fps, st.track_len / fps,
              st.disc_len / fps, player_volume_display(st.volume), st.buffer_pct, st.buffer_ms, st.underruns,
              st.read_errors, player_get_autoplay() ? "true" : "false", ota_progress(),
              st.preemphasis ? "true" : "false", st.deemphasis ? "true" : "false");
    jw_kv_str(w, "album", st.album);
    jw_kv_str(w, "artist", st.artist);
    jw_kv_str(w, "title", st.title);
    jw_kv_str(w, "performer", st.performer);
    jw_kv_str(w, "message", st.message);

    wifi_ap_record_t ap;
    int rssi = (net.connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    jw_printf(w, ",\"wifi\":{\"connected\":%s,\"rssi\":%d", net.connected ? "true" : "false", rssi);
    jw_kv_str(w, "ssid", net.ssid);
    jw_kv_str(w, "ip", net.ip);
    jw_kv_str(w, "hostname", CONFIG_CD_HOSTNAME);
    jw_printf(w, ",\"configured\":%s,\"ap\":{\"active\":%s,\"clients\":%d", net.configured ? "true" : "false",
              net.ap_active ? "true" : "false", net.ap_clients);
    jw_kv_str(w, "ssid", net.ap_ssid);
    jw_kv_str(w, "ip", NET_AP_IP_STR);
    jw_raw(w, "}", 1);

    const esp_app_desc_t *app = esp_app_get_description();
    jw_printf(w, "},\"fw\":{\"uptime\":%"PRIu64, (uint64_t)(esp_timer_get_time() / 1000000));
    jw_kv_str(w, "version", app->version);
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    jw_kv_str(w, "built", built);
    jw_kv_str(w, "idf", app->idf_ver);
    char elf[17];
    for (int i = 0; i < 8; i++) {
        snprintf(elf + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    }
    jw_kv_str(w, "elf", elf);
    jw_kv_str(w, "partition", esp_ota_get_running_partition()->label);
    jw_kv_str(w, "state", ota_running_state_name());
    jw_kv_str(w, "reset", ota_reset_reason_name());
    jw_printf(w, ",\"auth\":%s", auth_enabled() ? "true" : "false");

    size_t fs_total = 0, fs_used = 0;
    storage_get_info(&fs_total, &fs_used);
    meta_info_t mi;
    meta_get_info(&mi);
    jw_printf(w, "},\"meta\":{\"cover\":%s", mi.has_cover ? "true" : "false");
    jw_kv_str(w, "discid", mi.discid);
    jw_kv_str(w, "state", meta_state_name(mi.state));
    jw_kv_str(w, "release", mi.release);
    jw_kv_str(w, "error", mi.error);

    ui_backlight_cfg_t bl;
    ui_get_backlight(&bl);
    jw_printf(w, "},\"display\":{\"on\":%d,\"dim\":%d,\"dim_delay\":%d,\"off_delay\":%d", bl.on_level,
              bl.dim_level, bl.dim_delay_s, bl.off_delay_s);
    jw_kv_str(w, "mode", ui_dim_mode_name(bl.mode));

    jw_printf(w, "},\"sys\":{\"heap_internal\":%u,\"heap_internal_min\":%u,\"heap_psram\":%u,"
              "\"fs_used\":%u,\"fs_total\":%u}}",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)fs_used, (unsigned)fs_total);
    esp_err_t err = jw_end(w);
    free(w);
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    return send_status(req);
}

static esp_err_t tracks_handler(httpd_req_t *req)
{
    player_track_info_t *tracks = malloc(ATAPI_MAX_TRACKS * sizeof(*tracks));
    jw_t *w = malloc(sizeof(*w));
    if (!tracks || !w) {
        free(tracks);
        free(w);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    int n = player_get_tracks(tracks, ATAPI_MAX_TRACKS);
    jw_begin(w, req);
    jw_raw(w, "[", 1);
    for (int i = 0; i < n; i++) {
        jw_printf(w, "%s{\"n\":%d,\"audio\":%s,\"len\":%"PRIu32, i ? "," : "", tracks[i].number,
                  tracks[i].audio ? "true" : "false", tracks[i].length / ATAPI_CD_FRAMES_PER_SECOND);
        jw_kv_str(w, "title", tracks[i].title);
        jw_kv_str(w, "performer", tracks[i].performer);
        jw_raw(w, "}", 1);
    }
    jw_raw(w, "]", 1);
    esp_err_t err = jw_end(w);
    free(tracks);
    free(w);
    return err;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char query[64] = "", c[16] = "", v[16] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "c", c, sizeof(c));
    httpd_query_key_value(query, "v", v, sizeof(v));
    int arg = atoi(v);

    if (!strcmp(c, "play")) {
        player_command(PLAYER_CMD_PLAY_PAUSE, 0);
    } else if (!strcmp(c, "stop")) {
        player_command(PLAYER_CMD_STOP, 0);
    } else if (!strcmp(c, "next")) {
        player_command(PLAYER_CMD_NEXT, 0);
    } else if (!strcmp(c, "prev")) {
        player_command(PLAYER_CMD_PREV, 0);
    } else if (!strcmp(c, "skip")) {
        player_command(PLAYER_CMD_SEEK_REL, arg);
    } else if (!strcmp(c, "seek")) {
        player_command(PLAYER_CMD_SEEK_ABS, arg);
    } else if (!strcmp(c, "track")) {
        player_command(PLAYER_CMD_TRACK, arg);
    } else if (!strcmp(c, "volume")) { /* displayed percent 0..100 */
        int raw = (MIN(MAX(arg, 0), 100) * PLAYER_VOLUME_MAX + 50) / 100;
        player_command(PLAYER_CMD_VOLUME_SET, raw);
    } else if (!strcmp(c, "eject")) {
        app_eject_toggle();
    } else if (!strcmp(c, "meta")) {
        meta_refresh();
    } else if (!strcmp(c, "setup_ap")) {
        if (!auth_check(req)) {
            return ESP_OK;
        }
        net_setup_ap(arg != 0);
    } else if (!strcmp(c, "meta_forget")) {
        if (!auth_check(req)) {
            return ESP_OK;
        }
        meta_forget();
    } else if (!strcmp(c, "speed")) { /* test: v = kB/s (0 = max), m = PLAYER_SPEED_* bits (default 1) */
        char m[8] = "1";
        httpd_query_key_value(query, "m", m, sizeof(m));
        player_set_read_speed(MIN(MAX(arg, 0), 0xFFFF), atoi(m));
    } else if (!strcmp(c, "autoplay")) {
        player_set_autoplay(arg != 0);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
    }
    return send_status(req);
}

/** In-place URL decoding of a form value ('+' = space, %XX) */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    if (!auth_check(req)) {
        return ESP_OK;
    }
    char body[256];
    char ssid[3 * 32 + 1] = "", pass[3 * 64 + 1] = ""; /* URL-encoded sizes */
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    }
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    if (strlen(ssid) > 32 || strlen(pass) > 63) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID or password too long");
    }
    ESP_LOGI(TAG, "Wi-Fi credentials changed from the web UI (SSID \"%s\")", ssid);
    httpd_resp_sendstr(req, ssid[0] ? "Saved. Reconnecting; the page will be unreachable if the new network "
                       "is different or the password is wrong.\n" : "Wi-Fi credentials cleared.\n");
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the response go out before disconnecting */
    net_set_credentials(ssid, pass);
    return ESP_OK;
}

static esp_err_t cover_handler(httpd_req_t *req)
{
    char path[64];
    FILE *f = meta_cover_path(path, sizeof(path)) ? fopen(path, "rb") : NULL;
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No cover");
    }
    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400"); /* the page adds the disc ID to the URL */
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, 4096, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    const int max = 24;
    net_scan_entry_t *list = malloc(max * sizeof(*list));
    jw_t *w = malloc(sizeof(*w));
    int n = list && w ? net_scan(list, max) : -1;
    if (n < 0) {
        free(list);
        free(w);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Scan not possible right now (connecting?), try again\n");
    }
    jw_begin(w, req);
    jw_raw(w, "[", 1);
    for (int i = 0; i < n; i++) {
        jw_printf(w, "%s{\"rssi\":%d,\"open\":%s", i ? "," : "", list[i].rssi, list[i].open ? "true" : "false");
        jw_kv_str(w, "ssid", list[i].ssid);
        jw_raw(w, "}", 1);
    }
    jw_raw(w, "]", 1);
    esp_err_t err = jw_end(w);
    free(list);
    free(w);
    return err;
}

/** Captive portal: phones probe URLs like /generate_204 or /hotspot-detect.html; anything unknown goes to the UI */
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strncmp(req->uri, "/favicon", 8) == 0 || strncmp(req->uri, "/api/", 5) == 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    /* On the AP the probe's host name points to us via the captive DNS, but use the IP to be explicit;
     * on the home network a relative redirect keeps the host the user typed */
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    bool via_ap = getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) == 0 &&
                  local.sin_family == AF_INET && local.sin_addr.s_addr == ESP_IP4TOADDR(192, 168, 4, 1);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", via_ap ? "http://" NET_AP_IP_STR "/" : "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t admin_handler(httpd_req_t *req)
{
    if (!auth_check(req)) {
        return ESP_OK;
    }
    char body[3 * AUTH_PASSWORD_MAX + 16];
    char pass[3 * AUTH_PASSWORD_MAX + 1] = "";
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    }
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(pass);
    if (auth_set_password(pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password too long");
    }
    return httpd_resp_sendstr(req, pass[0] ? "Admin password set\n" : "Admin password removed\n");
}

/** Backlight settings; fields left out keep their value */
static esp_err_t display_handler(httpd_req_t *req)
{
    char body[160], v[16];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    }
    ui_backlight_cfg_t c;
    ui_get_backlight(&c);
    if (httpd_query_key_value(body, "mode", v, sizeof(v)) == ESP_OK) {
        c.mode = !strcmp(v, "never") ? UI_DIM_NEVER : !strcmp(v, "always") ? UI_DIM_ALWAYS : UI_DIM_IDLE;
    }
    if (httpd_query_key_value(body, "on", v, sizeof(v)) == ESP_OK) {
        c.on_level = atoi(v);
    }
    if (httpd_query_key_value(body, "dim", v, sizeof(v)) == ESP_OK) {
        c.dim_level = atoi(v);
    }
    if (httpd_query_key_value(body, "dim_delay", v, sizeof(v)) == ESP_OK) {
        c.dim_delay_s = atoi(v);
    }
    if (httpd_query_key_value(body, "off_delay", v, sizeof(v)) == ESP_OK) {
        c.off_delay_s = atoi(v);
    }
    if (ui_set_backlight(&c) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Applied, but saving failed");
    }
    return send_status(req);
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!auth_check(req)) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Reboot requested from the web UI");
    httpd_resp_sendstr(req, "Rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_register(httpd_handle_t server)
{
    const httpd_uri_t uris[WEB_URI_HANDLERS] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/tracks", .method = HTTP_GET, .handler = tracks_handler},
        {.uri = "/api/cmd", .method = HTTP_POST, .handler = cmd_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_handler},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/api/admin", .method = HTTP_POST, .handler = admin_handler},
        {.uri = "/api/display", .method = HTTP_POST, .handler = display_handler},
        {.uri = "/api/cover", .method = HTTP_GET, .handler = cover_handler},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = scan_handler},
    };
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);
    for (size_t i = 0; i < WEB_URI_HANDLERS; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Registering %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
