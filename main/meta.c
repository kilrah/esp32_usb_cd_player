/*
 * Album metadata and cover art: MusicBrainz disc ID lookup + Cover Art Archive, cached in LittleFS.
 *
 * Lookup:  https://musicbrainz.org/ws/2/discid/<id>?toc=...  (the TOC enables fuzzy matching when the
 *          exact disc ID isn't in the database). The best release is the one that lists this disc ID,
 *          has front cover art and the same track count.
 * Cover:   https://coverartarchive.org/release/<mbid>/front-250 (or the release group's), a ~250 px
 *          JPEG, redirected to archive.org. Decoded and scaled to META_COVER_SIZE for the display.
 * Cache:   /data/meta/<discid>.json (metadata) and <discid>.jpg (cover as downloaded)
 *
 * Everything network/file related runs in the "meta" task. Responses are kept in PSRAM, cJSON allocates
 * from PSRAM too (internal RAM is tight with Wi-Fi + TLS).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "jpeg_decoder.h"
#include "player.h"
#include "net.h"
#include "storage.h"
#include "meta.h"

static const char *TAG = "meta";

#define META_DIR            STORAGE_BASE_PATH "/meta"
#define MB_URL_FMT          "https://musicbrainz.org/ws/2/discid/%s?toc=%s&inc=recordings+artist-credits+release-groups&cdstubs=no&fmt=json"
#define CAA_URL_FMT         "https://coverartarchive.org/%s/%s/front-250"
#define MB_MAX_BYTES        (1024 * 1024)
#define COVER_MAX_BYTES     (512 * 1024)
static const int RETRY_DELAY_S[] = {5, 15, 60, 180}; /* after attempt 1, 2, 3, 4; transient failures right after boot are common */
#define MAX_ATTEMPTS        (int)(sizeof(RETRY_DELAY_S) / sizeof(RETRY_DELAY_S[0]) + 1)
#define NOT_FOUND_MEMORY    8   /* discs remembered as unknown until reboot, so they aren't looked up on every insert */

static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    /* current disc */
    char discid[DISCID_LEN + 1];
    char toc[DISCID_TOC_MAX];
    int first_track;
    int num_tracks;
    bool has_cdtext;
    /* jobs */
    bool job_cache;
    bool job_online;
    bool force;
    int attempts;
    int64_t retry_at;
    /* results */
    meta_state_t state;
    char release[40];
    bool has_cover;
    uint16_t *cover;        /* META_COVER_SIZE² RGB565, PSRAM */
    uint32_t cover_gen;
    char error[96];         /* reason of the last failed lookup */
    char not_found[NOT_FOUND_MEMORY][DISCID_LEN + 1];
    int not_found_next;
} M;

#define LOCK()   xSemaphoreTake(M.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(M.lock)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static char s_task_err[96]; /* failure reason, written by the meta task only */

static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

/** strlcpy that doesn't cut a UTF-8 sequence in half */
static void utf8_copy(char *dst, const char *src, size_t size)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--; /* src[n] is a continuation byte: drop the whole sequence */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/** MusicBrainz artist credit: names joined with their join phrases */
static void artist_credit(const cJSON *obj, char *out, size_t size)
{
    out[0] = '\0';
    char buf[2 * PLAYER_TEXT_LEN] = "";
    const cJSON *credit;
    cJSON_ArrayForEach(credit, cJSON_GetObjectItemCaseSensitive(obj, "artist-credit")) {
        const char *name = json_str(credit, "name");
        const char *join = json_str(credit, "joinphrase");
        strlcat(buf, name ? name : "", sizeof(buf));
        strlcat(buf, join ? join : "", sizeof(buf));
    }
    utf8_copy(out, buf, size);
}

static void cache_path(char *path, size_t size, const char *discid, const char *ext)
{
    snprintf(path, size, META_DIR "/%s.%s", discid, ext);
}

static esp_err_t read_file(const char *path, uint8_t **data, size_t *len, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    esp_err_t err = ESP_OK;
    if (size <= 0 || (size_t)size > max) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (!(*data = psram_malloc(size + 1))) {
        err = ESP_ERR_NO_MEM;
    } else if (fread(*data, 1, size, f) != (size_t)size) {
        free(*data);
        err = ESP_FAIL;
    } else {
        (*data)[size] = 0;
        *len = size;
    }
    fclose(f);
    return err;
}

static esp_err_t write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t n = fwrite(data, 1, len, f);
    return (fclose(f) == 0 && n == len) ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------------ */
/* HTTP                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t len, cap, max;
    bool overflow;
} body_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    /* Only keep the final response's body (not the bodies of redirects) */
    if (e->event_id != HTTP_EVENT_ON_DATA || esp_http_client_get_status_code(e->client) != 200 || b->overflow) {
        return ESP_OK;
    }
    if (b->len + e->data_len + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, (size_t)16384);
        while (cap < b->len + e->data_len + 1) {
            cap *= 2;
        }
        if (cap > b->max + 1) {
            cap = b->max + 1;
        }
        if (b->len + e->data_len + 1 > cap) {
            b->overflow = true;
            return ESP_OK;
        }
        uint8_t *nb = heap_caps_realloc(b->buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) {
            b->overflow = true;
            return ESP_OK;
        }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, e->data, e->data_len);
    b->len += e->data_len;
    b->buf[b->len] = 0;
    return ESP_OK;
}

/**
 * GET @p url (following redirects). On HTTP 200 the body is returned in @p body (PSRAM, NUL-terminated,
 * caller frees). @return ESP_OK, ESP_ERR_NOT_FOUND (HTTP 404), or an error
 */
static esp_err_t http_get(const char *url, const char *accept, size_t max, uint8_t **body, size_t *len)
{
    static char user_agent[96];
    if (!user_agent[0]) {
        /* MusicBrainz asks for "app/version ( contact )" */
        snprintf(user_agent, sizeof(user_agent), "usbhost_cd/%s ( %s )", esp_app_get_description()->version,
                 CONFIG_CD_META_CONTACT[0] ? CONFIG_CD_META_CONTACT : "ESP32-S3 USB CD player");
    }
    body_t b = {.max = max};
    esp_http_client_config_t cfg = {
        .url = url,
        .user_agent = user_agent,
        .timeout_ms = 20000,
        .max_redirection_count = 10,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &b,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", accept);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "GET %s: %s, HTTP %d, %u bytes, %d ms", url, esp_err_to_name(err), status, (unsigned)b.len,
             (int)((esp_timer_get_time() - t0) / 1000));
    if (err == ESP_OK && status == 200 && !b.overflow && b.len > 0) {
        *body = b.buf;
        *len = b.len;
        return ESP_OK;
    }
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    snprintf(s_task_err, sizeof(s_task_err), "%.*s: %s, HTTP %d%s", (int)strcspn(host, "/"), host,
             esp_err_to_name(err), status, b.overflow ? ", response too large" : "");
    free(b.buf);
    if (err != ESP_OK) {
        return err;
    }
    if (status == 404) {
        return ESP_ERR_NOT_FOUND;
    }
    return b.overflow ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
}

/* ------------------------------------------------------------------------ */
/* Cover image                                                              */
/* ------------------------------------------------------------------------ */

/** Decode a JPEG, center-crop it to a square and area-average it down to META_COVER_SIZE² RGB565 */
static uint16_t *cover_from_jpeg(const uint8_t *jpg, size_t len)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "Cover: not a supported JPEG");
        return NULL;
    }
    /* Largest decoder downscale that still leaves at least META_COVER_SIZE pixels */
    static const esp_jpeg_image_scale_t scales[] = {JPEG_IMAGE_SCALE_1_8, JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_2};
    static const int divs[] = {8, 4, 2};
    int div = 1;
    for (int i = 0; i < 3; i++) {
        if (MIN(info.width, info.height) / divs[i] >= META_COVER_SIZE) {
            cfg.out_scale = scales[i];
            div = divs[i];
            break;
        }
    }
    const int w = info.width / div, h = info.height / div;
    cfg.outbuf_size = (w + 1) * (h + 1) * 3;
    cfg.outbuf = psram_malloc(cfg.outbuf_size);
    uint16_t *out = psram_malloc(META_COVER_SIZE * META_COVER_SIZE * 2);
    esp_jpeg_image_output_t img;
    if (!cfg.outbuf || !out || esp_jpeg_decode(&cfg, &img) != ESP_OK) {
        ESP_LOGW(TAG, "Cover: decoding %ux%u failed (progressive JPEG?)", info.width, info.height);
        free(cfg.outbuf);
        free(out);
        return NULL;
    }

    const int side = MIN(img.width, img.height), ox = (img.width - side) / 2, oy = (img.height - side) / 2;
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
    for (int dy = 0; dy < META_COVER_SIZE; dy++) {
        int y0 = oy + dy * side / META_COVER_SIZE, y1 = MAX(oy + (dy + 1) * side / META_COVER_SIZE, y0 + 1);
        for (int dx = 0; dx < META_COVER_SIZE; dx++) {
            int x0 = ox + dx * side / META_COVER_SIZE, x1 = MAX(ox + (dx + 1) * side / META_COVER_SIZE, x0 + 1);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *p = cfg.outbuf + ((size_t)y * img.width + x0) * 3;
                for (int x = x0; x < x1; x++, p += 3, n++) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            r /= n;
            g /= n;
            b /= n;
            sum_r += r;
            sum_g += g;
            sum_b += b;
            out[dy * META_COVER_SIZE + dx] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
    free(cfg.outbuf);
    const int px = META_COVER_SIZE * META_COVER_SIZE;
    ESP_LOGI(TAG, "Cover %ux%u (decoded at 1/%d) -> %dx%d, average RGB %d,%d,%d", info.width, info.height, div,
             META_COVER_SIZE, META_COVER_SIZE, (int)(sum_r / px), (int)(sum_g / px), (int)(sum_b / px));
    return out;
}

/* ------------------------------------------------------------------------ */
/* MusicBrainz                                                              */
/* ------------------------------------------------------------------------ */

static bool medium_has_disc(const cJSON *medium, const char *discid)
{
    const cJSON *disc;
    cJSON_ArrayForEach(disc, cJSON_GetObjectItemCaseSensitive(medium, "discs")) {
        const char *id = json_str(disc, "id");
        if (id && !strcmp(id, discid)) {
            return true;
        }
    }
    return false;
}

typedef struct {
    player_meta_t *meta;
    char release[40];
    char release_group[40];
    bool front;
} mb_result_t;

/** Pick the best release/medium from a discid response and fill @p res */
static esp_err_t mb_parse(const char *json, const char *discid, int first_track, int num_tracks, mb_result_t *res)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "MusicBrainz: invalid JSON");
        snprintf(s_task_err, sizeof(s_task_err), "MusicBrainz: invalid JSON (out of memory?)");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *best_rel = NULL, *best_med = NULL, *rel;
    int best_score = -1;
    cJSON_ArrayForEach(rel, cJSON_GetObjectItemCaseSensitive(root, "releases")) {
        const cJSON *med, *pick = NULL;
        bool exact = false;
        cJSON_ArrayForEach(med, cJSON_GetObjectItemCaseSensitive(rel, "media")) {
            if (medium_has_disc(med, discid)) {
                pick = med;
                exact = true;
                break;
            }
            const cJSON *tc = cJSON_GetObjectItemCaseSensitive(med, "track-count");
            if (!pick && cJSON_IsNumber(tc) && tc->valueint == num_tracks) {
                pick = med;
            }
        }
        if (!pick) {
            continue;
        }
        const cJSON *caa = cJSON_GetObjectItemCaseSensitive(rel, "cover-art-archive");
        bool front = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(caa, "front"));
        int score = (exact ? 4 : 0) + (front ? 2 : 0);
        if (score > best_score) {
            best_score = score;
            best_rel = rel;
            best_med = pick;
        }
    }
    if (!best_rel) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    player_meta_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM);
    if (!m) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    utf8_copy(m->album, json_str(best_rel, "title"), sizeof(m->album));
    artist_credit(best_rel, m->artist, sizeof(m->artist));
    const cJSON *trk;
    cJSON_ArrayForEach(trk, cJSON_GetObjectItemCaseSensitive(best_med, "tracks")) {
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(trk, "position");
        int n = cJSON_IsNumber(pos) ? first_track + pos->valueint - 1 : 0;
        if (n < 1 || n > ATAPI_MAX_TRACKS) {
            continue;
        }
        utf8_copy(m->track[n].title, json_str(trk, "title"), sizeof(m->track[n].title));
        artist_credit(trk, m->track[n].artist, sizeof(m->track[n].artist));
    }
    res->meta = m;
    strlcpy(res->release, json_str(best_rel, "id") ?: "", sizeof(res->release));
    const cJSON *rg = cJSON_GetObjectItemCaseSensitive(best_rel, "release-group");
    strlcpy(res->release_group, json_str(rg, "id") ?: "", sizeof(res->release_group));
    res->front = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                  cJSON_GetObjectItemCaseSensitive(best_rel, "cover-art-archive"), "front"));
    ESP_LOGI(TAG, "MusicBrainz: \"%s\" by \"%s\", release %s%s%s", m->album, m->artist, res->release,
             best_score >= 4 ? " (exact disc ID)" : " (TOC match)", res->front ? ", has cover" : "");
    cJSON_Delete(root);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Cache                                                                    */
/* ------------------------------------------------------------------------ */

static void cache_save(const char *discid, const mb_result_t *res, const uint8_t *jpg, size_t jpg_len)
{
    char path[64];
    cache_path(path, sizeof(path), discid, "jpg");
    if (jpg) {
        if (write_file(path, jpg, jpg_len) != ESP_OK) {
            ESP_LOGW(TAG, "Writing %s failed", path);
            jpg = NULL;
        }
    } else {
        unlink(path);
    }

    const player_meta_t *m = res->meta;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "discid", discid);
    cJSON_AddStringToObject(root, "source", "musicbrainz");
    cJSON_AddStringToObject(root, "release", res->release);
    cJSON_AddStringToObject(root, "release_group", res->release_group);
    cJSON_AddStringToObject(root, "album", m->album);
    cJSON_AddStringToObject(root, "artist", m->artist);
    cJSON_AddBoolToObject(root, "cover", jpg != NULL);
    cJSON *tracks = cJSON_AddArrayToObject(root, "tracks");
    for (int n = 1; n <= ATAPI_MAX_TRACKS; n++) {
        if (m->track[n].title[0] || m->track[n].artist[0]) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "n", n);
            cJSON_AddStringToObject(t, "title", m->track[n].title);
            cJSON_AddStringToObject(t, "artist", m->track[n].artist);
            cJSON_AddItemToArray(tracks, t);
        }
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    cache_path(path, sizeof(path), discid, "json");
    if (!text || write_file(path, text, strlen(text)) != ESP_OK) {
        ESP_LOGW(TAG, "Writing %s failed", path);
    } else {
        ESP_LOGI(TAG, "Cached %s%s", path, jpg ? " + cover" : "");
    }
    free(text);
}

/** Load a cache entry. @return ESP_ERR_NOT_FOUND if there is none */
static esp_err_t cache_load(const char *discid, player_meta_t **meta, uint16_t **cover, char *release, size_t rel_size)
{
    char path[64];
    uint8_t *text = NULL;
    size_t len = 0;
    cache_path(path, sizeof(path), discid, "json");
    esp_err_t err = read_file(path, &text, &len, 256 * 1024);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse((const char *)text);
    free(text);
    player_meta_t *m = root ? heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM) : NULL;
    if (!m) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Cache entry %s unreadable", path);
        return ESP_ERR_INVALID_RESPONSE;
    }
    utf8_copy(m->album, json_str(root, "album"), sizeof(m->album));
    utf8_copy(m->artist, json_str(root, "artist"), sizeof(m->artist));
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "tracks")) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(t, "n");
        if (cJSON_IsNumber(n) && n->valueint >= 1 && n->valueint <= ATAPI_MAX_TRACKS) {
            utf8_copy(m->track[n->valueint].title, json_str(t, "title"), PLAYER_TEXT_LEN);
            utf8_copy(m->track[n->valueint].artist, json_str(t, "artist"), PLAYER_TEXT_LEN);
        }
    }
    strlcpy(release, json_str(root, "release") ?: "", rel_size);
    bool has_cover = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "cover"));
    cJSON_Delete(root);

    *cover = NULL;
    if (has_cover) {
        uint8_t *jpg = NULL;
        cache_path(path, sizeof(path), discid, "jpg");
        if (read_file(path, &jpg, &len, COVER_MAX_BYTES) == ESP_OK) {
            *cover = cover_from_jpeg(jpg, len);
            free(jpg);
        }
    }
    *meta = m;
    ESP_LOGI(TAG, "From cache: \"%s\" by \"%s\"%s", m->album, m->artist, *cover ? ", with cover" : "");
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Task                                                                     */
/* ------------------------------------------------------------------------ */

/** Hand results to the player/UI if the disc is still the same (takes ownership of meta and cover) */
static void apply(const char *discid, player_meta_t *meta, uint16_t *cover, const char *release, meta_state_t state)
{
    LOCK();
    if (strcmp(discid, M.discid) == 0) {
        player_set_metadata(meta);
        meta = NULL;
        if (cover) {
            memcpy(M.cover, cover, META_COVER_SIZE * META_COVER_SIZE * 2);
        }
        M.has_cover = cover != NULL;
        M.cover_gen++;
        strlcpy(M.release, release, sizeof(M.release));
        M.state = state;
    }
    UNLOCK();
    free(meta);
    free(cover);
}

static bool remembered_not_found(const char *discid)
{
    for (int i = 0; i < NOT_FOUND_MEMORY; i++) {
        if (!strcmp(M.not_found[i], discid)) {
            return true;
        }
    }
    return false;
}

static esp_err_t lookup_online(const char *discid, const char *toc, int first_track, int num_tracks)
{
    char url[sizeof(MB_URL_FMT) + DISCID_LEN + DISCID_TOC_MAX];
    snprintf(url, sizeof(url), MB_URL_FMT, discid, toc);
    uint8_t *body = NULL;
    size_t len = 0;
    esp_err_t err = http_get(url, "application/json", MB_MAX_BYTES, &body, &len);
    if (err != ESP_OK) {
        return err;
    }
    mb_result_t res = {0};
    err = mb_parse((const char *)body, discid, first_track, num_tracks, &res);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    if (res.front) {
        snprintf(url, sizeof(url), CAA_URL_FMT, "release", res.release);
        http_get(url, "image/jpeg", COVER_MAX_BYTES, &jpg, &jpg_len);
    }
    if (!jpg && res.release_group[0]) {
        snprintf(url, sizeof(url), CAA_URL_FMT, "release-group", res.release_group);
        http_get(url, "image/jpeg", COVER_MAX_BYTES, &jpg, &jpg_len);
    }
    uint16_t *cover = jpg ? cover_from_jpeg(jpg, jpg_len) : NULL;
    if (jpg && !cover) {
        free(jpg); /* undecodable (e.g. progressive): don't cache it */
        jpg = NULL;
    }
    cache_save(discid, &res, jpg, jpg_len);
    free(jpg);
    apply(discid, res.meta, cover, res.release, META_FOUND);
    return ESP_OK;
}

static void meta_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));

        LOCK();
        char discid[DISCID_LEN + 1], toc[DISCID_TOC_MAX];
        strlcpy(discid, M.discid, sizeof(discid));
        strlcpy(toc, M.toc, sizeof(toc));
        int first = M.first_track, num = M.num_tracks;
        bool do_cache = M.job_cache;
        M.job_cache = false;
        UNLOCK();
        if (!discid[0]) {
            continue;
        }

        if (do_cache) {
            player_meta_t *meta = NULL;
            uint16_t *cover = NULL;
            char release[40] = "";
            if (cache_load(discid, &meta, &cover, release, sizeof(release)) == ESP_OK) {
                apply(discid, meta, cover, release, META_FROM_CACHE);
                LOCK();
                if (!M.force && strcmp(discid, M.discid) == 0) {
                    M.job_online = false;
                }
                UNLOCK();
            }
        }

        LOCK();
        bool online = M.job_online && strcmp(discid, M.discid) == 0 && esp_timer_get_time() >= M.retry_at;
        if (M.job_online && M.state != META_FAILED && !net_is_connected()) {
            M.state = META_WAITING;
        }
        UNLOCK();
        if (!online || !net_is_connected()) {
            continue;
        }

        LOCK();
        M.state = META_LOOKING_UP;
        UNLOCK();
        ESP_LOGI(TAG, "Looking up disc %s online", discid);
        snprintf(s_task_err, sizeof(s_task_err), "unknown");
        esp_err_t err = lookup_online(discid, toc, first, num);

        LOCK();
        if (strcmp(discid, M.discid) == 0) {
            if (err == ESP_OK) {
                M.error[0] = '\0';
                M.job_online = false;
                M.force = false;
            } else if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGI(TAG, "Disc %s not found on MusicBrainz", discid);
                M.job_online = false;
                M.force = false;
                M.state = META_NOT_FOUND;
                strlcpy(M.not_found[M.not_found_next], discid, sizeof(M.not_found[0]));
                M.not_found_next = (M.not_found_next + 1) % NOT_FOUND_MEMORY;
            } else {
                M.state = META_FAILED;
                snprintf(M.error, sizeof(M.error), "%.30s (%.60s)", esp_err_to_name(err), s_task_err);
                if (++M.attempts >= MAX_ATTEMPTS) {
                    ESP_LOGW(TAG, "Lookup failed %d times (%s), giving up for this disc", M.attempts, M.error);
                    M.job_online = false;
                } else {
                    int delay = RETRY_DELAY_S[M.attempts - 1];
                    ESP_LOGW(TAG, "Lookup failed (%s), retry in %d s", M.error, delay);
                    M.retry_at = esp_timer_get_time() + (int64_t)delay * 1000000;
                }
            }
        }
        UNLOCK();
    }
}

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t meta_init(void)
{
    static cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    M.lock = xSemaphoreCreateMutex();
    M.cover = heap_caps_calloc(1, META_COVER_SIZE * META_COVER_SIZE * 2, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(M.lock && M.cover, ESP_ERR_NO_MEM, TAG, "");
    mkdir(META_DIR, 0775); /* fails harmlessly if it exists (or if storage isn't mounted) */
    /* Internal RAM stack: the task writes to flash (LittleFS), which disables the cache */
    ESP_RETURN_ON_FALSE(xTaskCreate(meta_task, "meta", 10240, NULL, 2, &M.task) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task");
    return ESP_OK;
}

void meta_disc_loaded(const atapi_toc_t *toc, bool has_cdtext)
{
    char discid[DISCID_LEN + 1], toc_str[DISCID_TOC_MAX];
    int n = discid_compute(toc, discid, toc_str, sizeof(toc_str));
    if (n == 0) {
        return;
    }
    ESP_LOGI(TAG, "Disc ID %s (%d audio tracks)%s", discid, n, has_cdtext ? ", has CD-TEXT" : "");
    LOCK();
    strlcpy(M.discid, discid, sizeof(M.discid));
    strlcpy(M.toc, toc_str, sizeof(M.toc));
    M.first_track = toc->tracks[0].number;
    M.num_tracks = n;
    M.has_cdtext = has_cdtext;
    M.has_cover = false;
    M.cover_gen++;
    M.release[0] = '\0';
    M.error[0] = '\0';
    M.force = false;
    M.attempts = 0;
    M.retry_at = 0;
    /* Cache first, then online. Discs with CD-TEXT are looked up too (for the cover); their CD-TEXT
     * still takes precedence over the online text (player.c). */
    M.job_cache = true;
    M.job_online = !remembered_not_found(discid);
    M.state = remembered_not_found(discid) ? META_NOT_FOUND : META_WAITING;
    UNLOCK();
    xTaskNotifyGive(M.task);
}

void meta_disc_unloaded(void)
{
    LOCK();
    M.discid[0] = '\0';
    M.job_cache = M.job_online = M.force = false;
    M.has_cover = false;
    M.cover_gen++;
    M.release[0] = '\0';
    M.state = META_IDLE;
    UNLOCK();
}

void meta_refresh(void)
{
    LOCK();
    if (M.discid[0]) {
        M.force = true;
        M.job_online = true;
        M.attempts = 0;
        M.retry_at = 0;
        M.state = META_WAITING;
        for (int i = 0; i < NOT_FOUND_MEMORY; i++) {
            if (!strcmp(M.not_found[i], M.discid)) {
                M.not_found[i][0] = '\0';
            }
        }
    }
    UNLOCK();
    xTaskNotifyGive(M.task);
}

void meta_forget(void)
{
    char json[64] = "", jpg[64] = "";
    LOCK();
    if (M.discid[0]) {
        cache_path(json, sizeof(json), M.discid, "json");
        cache_path(jpg, sizeof(jpg), M.discid, "jpg");
    }
    UNLOCK();
    if (json[0]) {
        unlink(json);
        unlink(jpg);
        ESP_LOGI(TAG, "Cache entry deleted: %s", json);
    }
}

void meta_get_info(meta_info_t *info)
{
    LOCK();
    strlcpy(info->discid, M.discid, sizeof(info->discid));
    info->state = M.state;
    info->has_cover = M.has_cover;
    strlcpy(info->release, M.release, sizeof(info->release));
    strlcpy(info->error, M.error, sizeof(info->error));
    UNLOCK();
}

const char *meta_state_name(meta_state_t state)
{
    static const char *names[] = {"idle", "waiting for Wi-Fi", "looking up", "from cache", "found online",
                                  "not found", "lookup failed"};
    return state <= META_FAILED ? names[state] : "?";
}

bool meta_get_cover(uint32_t *gen, uint16_t *buf, bool *has_cover)
{
    bool changed = false;
    if (!M.lock) {
        return false; /* meta_init() failed */
    }
    LOCK();
    if (*gen != M.cover_gen) {
        *gen = M.cover_gen;
        *has_cover = M.has_cover;
        if (M.has_cover) {
            memcpy(buf, M.cover, META_COVER_SIZE * META_COVER_SIZE * 2);
        }
        changed = true;
    }
    UNLOCK();
    return changed;
}

bool meta_cover_path(char *path, size_t size)
{
    LOCK();
    bool ok = M.discid[0] && M.has_cover;
    if (ok) {
        cache_path(path, size, M.discid, "jpg");
    }
    UNLOCK();
    return ok;
}

void meta_print_info(void)
{
    meta_info_t info;
    meta_get_info(&info);
    printf("META discid=%s state=\"%s\" cover=%s release=%s%s%s\n", info.discid[0] ? info.discid : "-",
           meta_state_name(info.state), info.has_cover ? "yes" : "no", info.release[0] ? info.release : "-",
           info.error[0] ? " last_error=" : "", info.error);
}
