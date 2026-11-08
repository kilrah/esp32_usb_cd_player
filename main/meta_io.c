/*
 * Metadata cache export / import as a tar archive, streamed over HTTP.
 *
 * Export: every file in the cache directory as meta/<name>, generated on the fly (512-byte ustar header per
 *         file, data padded to 512 bytes, two zero blocks at the end). No compression: the covers are JPEGs.
 * Import: the request body is parsed block by block. Only cache files are taken: "<28-char disc ID>.json"
 *         (must be a JSON object with that "discid") and "<disc ID>.jpg" (must start with a JPEG marker), from
 *         any directory in the archive. Existing files are replaced, everything else is kept. GNU long names and
 *         pax headers are understood, other entry types are skipped. Each file is written to a temporary name
 *         first and then renamed, so a broken upload never leaves a half-written cache file. Files identical to
 *         the stored ones aren't rewritten.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/param.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "discid.h"
#include "storage.h"
#include "meta.h"
#include "meta_io.h"

static const char *TAG = "meta_io";

#define BLOCK           512
#define IO_BUF_SIZE     4096
#define JSON_MAX_BYTES  (64 * 1024)
#define JPG_MAX_BYTES   (512 * 1024)        /* same limit as the cover download */
#define ARCHIVE_DIR     "meta/"

/* ------------------------------------------------------------------------ */
/* Export                                                                   */
/* ------------------------------------------------------------------------ */

static void put_octal(char *dst, size_t width, unsigned long long v)
{
    /* width - 1 digits, NUL terminated */
    dst[width - 1] = '\0';
    for (int i = width - 2; i >= 0; i--) {
        dst[i] = '0' + (v & 7);
        v >>= 3;
    }
}

static void make_header(uint8_t *h, const char *name, size_t size, char type)
{
    memset(h, 0, BLOCK);
    strlcpy((char *)h, name, 100);
    put_octal((char *)h + 100, 8, type == '5' ? 0755 : 0644); /* mode */
    put_octal((char *)h + 108, 8, 0);                          /* uid */
    put_octal((char *)h + 116, 8, 0);                          /* gid */
    put_octal((char *)h + 124, 12, size);
    put_octal((char *)h + 136, 12, 0);                         /* mtime: no clock on the device */
    h[156] = type;
    memcpy(h + 257, "ustar", 6);                               /* magic incl. NUL */
    memcpy(h + 263, "00", 2);                                  /* version */
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; i++) {
        sum += h[i];
    }
    put_octal((char *)h + 148, 7, sum);                        /* 6 digits + NUL, then the space stays */
}

esp_err_t meta_export_tar(httpd_req_t *req)
{
    DIR *dir = opendir(META_CACHE_DIR);
    uint8_t *buf = heap_caps_malloc(IO_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        if (dir) {
            closedir(dir);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(req, "application/x-tar");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"cdplayer-meta.tar\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    make_header(buf, ARCHIVE_DIR, 0, '5');
    esp_err_t err = httpd_resp_send_chunk(req, (char *)buf, BLOCK);
    int files = 0;
    size_t bytes = 0;
    struct dirent *de;
    while (err == ESP_OK && dir && (de = readdir(dir))) {
        char path[sizeof(META_CACHE_DIR) + 1 + sizeof(de->d_name)], name[sizeof(ARCHIVE_DIR) + sizeof(de->d_name)];
        if (strlen(de->d_name) > 100 - sizeof(ARCHIVE_DIR)) {
            continue; /* doesn't fit a plain ustar name; cache file names are 33 characters */
        }
        snprintf(path, sizeof(path), META_CACHE_DIR "/%s", de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        snprintf(name, sizeof(name), ARCHIVE_DIR "%s", de->d_name);
        size_t size = st.st_size;
        make_header(buf, name, size, '0');
        err = httpd_resp_send_chunk(req, (char *)buf, BLOCK);
        /* Exactly `size` bytes even if the file changed meanwhile (zero padded or truncated) */
        size_t left = (size + BLOCK - 1) / BLOCK * BLOCK, data_left = size;
        while (err == ESP_OK && left) {
            size_t n = MIN(left, (size_t)IO_BUF_SIZE);
            size_t got = data_left ? fread(buf, 1, MIN(n, data_left), f) : 0;
            data_left -= got;
            memset(buf + got, 0, n - got);
            err = httpd_resp_send_chunk(req, (char *)buf, n);
            left -= n;
        }
        fclose(f);
        files++;
        bytes += size;
    }
    if (dir) {
        closedir(dir);
    }
    if (err == ESP_OK) {
        memset(buf, 0, 2 * BLOCK);
        err = httpd_resp_send_chunk(req, (char *)buf, 2 * BLOCK);
    }
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Export aborted: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Exported %d files, %u bytes", files, (unsigned)bytes);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Import                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    httpd_req_t *req;
    size_t left;            /* body bytes not yet received */
} body_t;

static esp_err_t recv_exact(body_t *b, void *dst, size_t len)
{
    uint8_t *p = dst;
    int timeouts = 0;
    while (len) {
        if (!b->left) {
            return ESP_ERR_INVALID_SIZE; /* truncated archive */
        }
        int n = httpd_req_recv(b->req, (char *)p, MIN(len, b->left));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        p += n;
        len -= n;
        b->left -= n;
    }
    return ESP_OK;
}

static esp_err_t skip(body_t *b, size_t len, uint8_t *scratch)
{
    while (len) {
        size_t n = MIN(len, (size_t)IO_BUF_SIZE);
        ESP_RETURN_ON_ERROR(recv_exact(b, scratch, n), TAG, "");
        len -= n;
    }
    return ESP_OK;
}

static unsigned long long get_octal(const uint8_t *p, size_t width)
{
    unsigned long long v = 0;
    size_t i = 0;
    while (i < width && (p[i] == ' ' || p[i] == '\0')) {
        i++;
    }
    for (; i < width && p[i] >= '0' && p[i] <= '7'; i++) {
        v = (v << 3) | (p[i] - '0');
    }
    return v;
}

static bool header_valid(const uint8_t *h)
{
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; i++) {
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    }
    return sum == get_octal(h + 148, 8);
}

/** Cache file name check: returns 'j' (.json), 'c' (.jpg) or 0. @p base receives the file name. */
static char cache_name(const char *path, char *base, size_t base_size)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    strlcpy(base, name, base_size);
    const char *dot = strrchr(name, '.');
    if (!dot || dot - name != DISCID_LEN) {
        return 0;
    }
    for (const char *c = name; c < dot; c++) {
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') {
            return 0;
        }
    }
    return !strcmp(dot, ".json") ? 'j' : !strcmp(dot, ".jpg") ? 'c' : 0;
}

/** "path=" record of a pax extended header (records are "<len> key=value\n") */
static void pax_path(const char *data, size_t len, char *out, size_t out_size)
{
    size_t pos = 0;
    while (pos < len) {
        char *end;
        unsigned long rec = strtoul(data + pos, &end, 10);
        if (rec == 0 || pos + rec > len || *end != ' ') {
            return;
        }
        const char *kv = end + 1, *rec_end = data + pos + rec - 1; /* without the '\n' */
        if (rec_end > kv + 5 && !strncmp(kv, "path=", 5)) {
            size_t n = MIN((size_t)(rec_end - kv - 5), out_size - 1);
            memcpy(out, kv + 5, n);
            out[n] = '\0';
        }
        pos += rec;
    }
}

static bool content_ok(char kind, const char *base, const uint8_t *data, size_t len)
{
    if (kind == 'c') {
        return len > 4 && data[0] == 0xFF && data[1] == 0xD8;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "discid");
    bool ok = cJSON_IsObject(root) && cJSON_IsString(id) && !strncmp(id->valuestring, base, DISCID_LEN) &&
              strlen(id->valuestring) == DISCID_LEN;
    cJSON_Delete(root);
    return ok;
}

static bool same_as_stored(const char *path, const uint8_t *data, size_t len, uint8_t *scratch)
{
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size != len) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool same = true;
    for (size_t pos = 0; same && pos < len;) {
        size_t n = fread(scratch, 1, MIN(len - pos, (size_t)IO_BUF_SIZE), f);
        same = n > 0 && memcmp(scratch, data + pos, n) == 0;
        pos += n;
    }
    fclose(f);
    return same;
}

/** Write a cache file (temporary name, then rename). ESP_ERR_INVALID_STATE: identical file already stored. */
static esp_err_t store(const char *base, const uint8_t *data, size_t len, uint8_t *scratch)
{
    char path[96], tmp[100];
    snprintf(path, sizeof(path), META_CACHE_DIR "/%s", base);
    if (same_as_stored(path, data, len, scratch)) {
        return ESP_ERR_INVALID_STATE; /* nothing to do: saves time and flash wear on a re-import */
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t n = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || n != len) {
        unlink(tmp);
        return ESP_FAIL;
    }
    unlink(path); /* replace */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t meta_import_tar(httpd_req_t *req, meta_import_result_t *res)
{
    memset(res, 0, sizeof(*res));
    body_t b = {.req = req, .left = req->content_len};
    uint8_t *hdr = heap_caps_malloc(BLOCK, MALLOC_CAP_SPIRAM);
    uint8_t *scratch = heap_caps_malloc(IO_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *long_name = heap_caps_calloc(1, 256, MALLOC_CAP_SPIRAM);
    esp_err_t err = (hdr && scratch && long_name) ? ESP_OK : ESP_ERR_NO_MEM;
    mkdir(META_CACHE_DIR, 0775);
    int zero_blocks = 0;

    while (err == ESP_OK && b.left) {
        if ((err = recv_exact(&b, hdr, BLOCK)) != ESP_OK) {
            break;
        }
        bool zero = true;
        for (int i = 0; i < BLOCK && zero; i++) {
            zero = hdr[i] == 0;
        }
        if (zero) {
            if (++zero_blocks == 2) {
                break; /* end of archive; ignore any trailing padding */
            }
            continue;
        }
        zero_blocks = 0;
        if (!header_valid(hdr)) {
            ESP_LOGW(TAG, "Not a tar archive (bad header checksum)");
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        size_t size = get_octal(hdr + 124, 12);
        size_t padded = (size + BLOCK - 1) / BLOCK * BLOCK;
        char type = hdr[156];

        if (type == 'L' || type == 'x') { /* GNU long name / pax header for the next entry */
            uint8_t *data = heap_caps_malloc(padded + 1, MALLOC_CAP_SPIRAM);
            if (!data || size > 8192) {
                free(data);
                err = skip(&b, padded, scratch);
                continue;
            }
            err = recv_exact(&b, data, padded);
            data[size] = '\0';
            if (err == ESP_OK && type == 'L') {
                strlcpy(long_name, (char *)data, 256);
            } else if (err == ESP_OK) {
                pax_path((char *)data, size, long_name, 256);
            }
            free(data);
            continue;
        }

        char name[260];
        if (long_name[0]) {
            strlcpy(name, long_name, sizeof(name));
            long_name[0] = '\0';
        } else {
            /* ustar: prefix (155 bytes at 345) + "/" + name (100 bytes) */
            char n100[101] = "", prefix[156] = "";
            memcpy(n100, hdr, 100);
            if (!memcmp(hdr + 257, "ustar", 5)) {
                memcpy(prefix, hdr + 345, 155);
            }
            snprintf(name, sizeof(name), "%s%s%s", prefix, prefix[0] ? "/" : "", n100);
        }

        char base[64];
        char kind = (type == '0' || type == '\0') ? cache_name(name, base, sizeof(base)) : 0;
        size_t max = kind == 'j' ? JSON_MAX_BYTES : JPG_MAX_BYTES;
        if (!kind || size == 0 || size > max) {
            if (type == '0' || type == '\0') {
                ESP_LOGI(TAG, "Skipped %s (%u bytes)", name, (unsigned)size);
                res->skipped++;
            }
            err = skip(&b, padded, scratch);
            continue;
        }
        uint8_t *data = heap_caps_malloc(padded, MALLOC_CAP_SPIRAM);
        if (!data) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        err = recv_exact(&b, data, padded);
        if (err == ESP_OK) {
            if (!content_ok(kind, base, data, size)) {
                ESP_LOGW(TAG, "Skipped %s: invalid content", name);
                res->skipped++;
            } else {
                esp_err_t e = store(base, data, size, scratch);
                if (e == ESP_ERR_INVALID_STATE) {
                    res->unchanged++;
                } else if (e != ESP_OK) {
                    ESP_LOGW(TAG, "Writing %s failed (storage full?)", base);
                    res->failed++;
                } else {
                    res->imported++;
                }
            }
        }
        free(data);
    }

    free(hdr);
    free(scratch);
    free(long_name);
    if (res->imported) {
        storage_changed();
        meta_reload_cache(); /* the current disc may have new info */
    }
    ESP_LOGI(TAG, "Import: %d files imported, %d unchanged, %d skipped, %d failed%s%s", res->imported,
             res->unchanged, res->skipped, res->failed,
             err != ESP_OK ? ", error: " : "", err != ESP_OK ? esp_err_to_name(err) : "");
    return err;
}
