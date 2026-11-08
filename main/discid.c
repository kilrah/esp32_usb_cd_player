/*
 * MusicBrainz disc ID: SHA-1 over the hex TOC, base64 with the alphabet "._-" substitutions.
 * A small SHA-1 is included because mbedTLS 4 in IDF 6 no longer exports it.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "discid.h"

#define PREGAP 150 /* MSF 00:02:00: LBA 0 = sector offset 150 */
#define DATA_TRACK_GAP 11400

/* ------------------------------------------------------------------------ */
/* SHA-1 (FIPS 180-1)                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} sha1_t;

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(sha1_t *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = ROL(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_t *c)
{
    static const uint32_t init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(c->h, init, sizeof(init));
    c->len = 0;
    c->fill = 0;
}

static void sha1_update(sha1_t *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    while (n--) {
        c->buf[c->fill++] = *p++;
        if (c->fill == 64) {
            sha1_block(c, c->buf);
            c->fill = 0;
        }
    }
}

static void sha1_final(sha1_t *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56) {
        sha1_update(c, &pad, 1);
    }
    for (int i = 7; i >= 0; i--) {
        uint8_t b = (uint8_t)(bits >> (8 * i));
        sha1_update(c, &b, 1);
    }
    for (int i = 0; i < 20; i++) {
        out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

/* ------------------------------------------------------------------------ */

static void sha1_hex(sha1_t *c, uint32_t v, int digits)
{
    char s[9];
    snprintf(s, sizeof(s), "%0*lX", digits, (unsigned long)v);
    sha1_update(c, s, digits);
}

int discid_compute(const atapi_toc_t *toc, char id[DISCID_LEN + 1], char *toc_str, size_t toc_str_size)
{
    int n = toc->num_tracks;
    uint32_t leadout = toc->leadout_lba;
    /* Enhanced CD: drop trailing data tracks, the audio session ends 11400 sectors before them */
    while (n > 1 && atapi_toc_entry_is_data(&toc->tracks[n - 1])) {
        leadout = toc->tracks[n - 1].lba - DATA_TRACK_GAP;
        n--;
    }
    if (n < 1 || (n == 1 && atapi_toc_entry_is_data(&toc->tracks[0]))) {
        return 0; /* no audio tracks */
    }
    int first = toc->tracks[0].number, last = toc->tracks[n - 1].number;

    sha1_t c;
    sha1_init(&c);
    sha1_hex(&c, first, 2);
    sha1_hex(&c, last, 2);
    sha1_hex(&c, leadout + PREGAP, 8);
    for (int t = 1; t <= 99; t++) {
        uint32_t off = 0;
        for (int i = 0; i < n; i++) {
            if (toc->tracks[i].number == t) {
                off = toc->tracks[i].lba + PREGAP;
            }
        }
        sha1_hex(&c, off, 8);
    }
    uint8_t digest[21] = {0};
    sha1_final(&c, digest);

    /* base64 with '+' -> '.', '/' -> '_', '=' -> '-' (20 bytes -> 28 chars incl. one pad) */
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    int o = 0;
    for (int i = 0; i < 21; i += 3) {
        uint32_t v = (uint32_t)digest[i] << 16 | (uint32_t)digest[i + 1] << 8 | digest[i + 2];
        id[o++] = alphabet[(v >> 18) & 63];
        id[o++] = alphabet[(v >> 12) & 63];
        id[o++] = alphabet[(v >> 6) & 63];
        id[o++] = alphabet[v & 63];
    }
    id[DISCID_LEN - 1] = '-'; /* 20 bytes = 6 full groups + 2 bytes: one pad character */
    id[DISCID_LEN] = '\0';

    if (toc_str && toc_str_size) {
        int len = snprintf(toc_str, toc_str_size, "%d+%d+%lu", first, last, (unsigned long)(leadout + PREGAP));
        for (int i = 0; i < n && len > 0 && (size_t)len < toc_str_size; i++) {
            len += snprintf(toc_str + len, toc_str_size - len, "+%lu", (unsigned long)(toc->tracks[i].lba + PREGAP));
        }
    }
    return n;
}
