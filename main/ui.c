/*
 * CD player screen (LVGL, 320x170 landscape)
 *
 *  +------------------------------------------------+
 *  | Artist - Album                     +--------+  |
 *  | Track title                   (20) | cover  |  |
 *  | Track 3 / 11                       | 72x72  |  |
 *  |                                    +--------+  |
 *  | 01:23                  / 04:17              > |
 *  | [=============-------------------------------] |
 *  | Vol 100%   Buf  92%   (wifi) (warn)     34:56 |
 *  +------------------------------------------------+
 *
 * The cover (meta.c: cache / Cover Art Archive) is only shown when there is one; the album and title
 * lines then end left of it. Text is transliterated to ASCII: the Montserrat fonts have no other glyphs.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "tdisplay_s3.h"
#include "player.h"
#include "net.h"
#include "ota.h"
#include "meta.h"
#include "ui.h"

static const char *TAG = "ui";

#define COL_X        10
#define COL_W        (TDS3_LCD_H_RES - 2 * COL_X)
#define COVER_X      (TDS3_LCD_H_RES - COL_X - META_COVER_SIZE)
#define COVER_Y      6
#define TEXT_W_COVER (COVER_X - 8 - COL_X)   /* album/title width when the cover is shown */
#define COLOR_DIM    lv_color_hex(0x8a8a8a)
#define COLOR_ACCENT lv_color_hex(0x37b4ff)

static lv_obj_t *s_album, *s_title, *s_track, *s_state, *s_time, *s_total, *s_bar, *s_disc_len;
static lv_obj_t *s_vol, *s_buf, *s_wifi, *s_warn; /* bottom line: fixed slots, values right-aligned */
static lv_obj_t *s_cover, *s_spinner;
static bool s_has_cover;
static uint16_t *s_cover_px;                     /* RGB565, PSRAM; only touched in the LVGL task */
static lv_image_dsc_t s_cover_dsc;
static uint32_t s_cover_gen = UINT32_MAX;

/* ------------------------------------------------------------------------ */
/* UTF-8 -> ASCII for the display                                           */
/* ------------------------------------------------------------------------ */

/* U+00C0..U+00FF and U+0100..U+017F to base letters ('?' = handled separately) */
static const char LATIN1[] = "AAAAAA?CEEEEIIIIDNOOOOOxOUUUUYT?aaaaaa?ceeeeiiiidnooooo/ouuuuyty";
static const char LATIN_EXT_A[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNn"
                                  "OoOoOo?\?RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
_Static_assert(sizeof(LATIN1) == 64 + 1, "U+00C0..U+00FF");
_Static_assert(sizeof(LATIN_EXT_A) == 128 + 1, "U+0100..U+017F");

static const char *ascii_for(uint32_t cp)
{
    static char one[2];
    switch (cp) {
    case 0x00C6: return "AE";
    case 0x00E6: return "ae";
    case 0x00DF: return "ss";
    case 0x0152: return "OE";
    case 0x0153: return "oe";
    case 0x00A0: return " ";
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x00B4: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x00B7: return "*";
    }
    one[1] = '\0';
    if (cp >= 0xC0 && cp <= 0xFF) {
        one[0] = LATIN1[cp - 0xC0];
    } else if (cp >= 0x100 && cp <= 0x17F) {
        one[0] = LATIN_EXT_A[cp - 0x100];
    } else {
        one[0] = '?';
    }
    return one;
}

static void to_ascii(char *dst, size_t size, const char *src)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && o + 1 < size) {
        uint32_t cp;
        int n;
        if (*p < 0x80) {
            cp = *p;
            n = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            n = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            n = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            n = 4;
        } else {
            cp = '?'; /* stray byte */
            n = 1;
        }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { /* truncated sequence */
                cp = '?';
                n = i;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += n;
        const char *rep = cp < 0x80 ? NULL : ascii_for(cp);
        if (!rep) {
            dst[o++] = (char)cp;
        } else {
            for (; *rep && o + 1 < size; rep++) {
                dst[o++] = *rep;
            }
        }
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------------ */

static void set_text(lv_obj_t *lbl, const char *text)
{
    /* Avoid resetting scroll animations when nothing changed */
    if (strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

static void set_text_ascii(lv_obj_t *lbl, const char *utf8)
{
    char buf[2 * PLAYER_TEXT_LEN + 8];
    to_ascii(buf, sizeof(buf), utf8);
    set_text(lbl, buf);
}

static void fmt_time(char *buf, size_t len, uint32_t sectors)
{
    uint32_t s = sectors / ATAPI_CD_FRAMES_PER_SECOND;
    snprintf(buf, len, "%02"PRIu32":%02"PRIu32, s / 60, s % 60);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
    }
    lv_label_set_text(l, "");
    return l;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** Cover image, and a spinner in its place (on top of an existing cover) while a lookup runs */
static void update_cover(void)
{
    if (s_cover_px && meta_get_cover(&s_cover_gen, s_cover_px, &s_has_cover) && s_has_cover) {
        /* Same descriptor, new pixels: raw RGB565 is drawn from memory (no image cache), so a redraw is enough */
        lv_image_set_src(s_cover, &s_cover_dsc);
        lv_obj_invalidate(s_cover);
    }
    meta_info_t mi;
    meta_get_info(&mi);
    bool looking_up = mi.state == META_LOOKING_UP;
    set_hidden(s_cover, !s_has_cover);
    set_hidden(s_spinner, !looking_up);

    int32_t w = (s_has_cover || looking_up) ? TEXT_W_COVER : COL_W;
    if (lv_obj_get_width(s_album) != w) {
        lv_obj_set_width(s_album, w);
        lv_obj_set_width(s_title, w);
    }
}

static void update_cb(lv_timer_t *t)
{
    player_status_t st;
    char buf[2 * PLAYER_TEXT_LEN + 8];
    player_get_status(&st);
    update_cover();

    if (st.state == PLAYER_NO_DISC) {
        set_text(s_album, "");
        set_text_ascii(s_title, st.message);
        set_text(s_track, "");
        set_text(s_state, "");
        set_text(s_time, "--:--");
        set_text(s_total, "");
        set_text(s_disc_len, "");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    } else {
        if (st.artist[0] && st.album[0]) {
            snprintf(buf, sizeof(buf), "%s - %s", st.artist, st.album);
        } else {
            snprintf(buf, sizeof(buf), "%s", st.artist[0] ? st.artist : st.album[0] ? st.album : "Audio CD");
        }
        set_text_ascii(s_album, buf);

        if (st.performer[0]) {
            snprintf(buf, sizeof(buf), "%s - %s", st.title, st.performer);
            set_text_ascii(s_title, buf);
        } else {
            set_text_ascii(s_title, st.title);
        }

        snprintf(buf, sizeof(buf), "Track %d / %d", st.track, st.num_tracks);
        set_text(s_track, buf);
        set_text(s_state, st.state == PLAYER_PLAYING ? LV_SYMBOL_PLAY :
                 st.state == PLAYER_PAUSED ? LV_SYMBOL_PAUSE : LV_SYMBOL_STOP);

        fmt_time(buf, sizeof(buf), st.track_pos);
        set_text(s_time, buf);
        buf[0] = '/';
        buf[1] = ' ';
        fmt_time(buf + 2, sizeof(buf) - 2, st.track_len);
        set_text(s_total, buf);
        fmt_time(buf, sizeof(buf), st.disc_len);
        set_text(s_disc_len, buf);
        lv_bar_set_value(s_bar, st.track_len ? (int32_t)((uint64_t)st.track_pos * 1000 / st.track_len) : 0,
                         LV_ANIM_OFF);
    }

    int ota = ota_progress();
    if (ota >= 0) {
        snprintf(buf, sizeof(buf), "FW update: %d%%", ota);
        set_text(s_title, buf);
    }

    net_status_t net;
    net_get_status(&net);
    snprintf(buf, sizeof(buf), "%d%%", player_volume_display(st.volume));
    set_text(s_vol, buf);
    snprintf(buf, sizeof(buf), "%d%%", st.buffer_pct);
    set_text(s_buf, buf);
    set_text(s_wifi, net.connected ? LV_SYMBOL_WIFI : "");
    set_text(s_warn, st.read_errors ? LV_SYMBOL_WARNING : "");
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Cover art, top right (hidden until there is one) */
    s_cover_px = heap_caps_calloc(1, META_COVER_SIZE * META_COVER_SIZE * 2, MALLOC_CAP_SPIRAM);
    s_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_cover_dsc.header.w = META_COVER_SIZE;
    s_cover_dsc.header.h = META_COVER_SIZE;
    s_cover_dsc.header.stride = META_COVER_SIZE * 2;
    s_cover_dsc.data_size = META_COVER_SIZE * META_COVER_SIZE * 2;
    s_cover_dsc.data = (const uint8_t *)s_cover_px;
    s_cover = lv_image_create(scr);
    lv_obj_set_pos(s_cover, COVER_X, COVER_Y);
    lv_obj_set_size(s_cover, META_COVER_SIZE, META_COVER_SIZE);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);

    /* Spinner in the cover area while album info is being looked up online */
    const int spin = 36;
    s_spinner = lv_spinner_create(scr);
    lv_spinner_set_anim_params(s_spinner, 1000, 240);
    lv_obj_set_size(s_spinner, spin, spin);
    lv_obj_set_pos(s_spinner, COVER_X + (META_COVER_SIZE - spin) / 2, COVER_Y + (META_COVER_SIZE - spin) / 2);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    /* Track info */
    s_album = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X, 6, COL_W);
    lv_label_set_long_mode(s_album, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_title = make_label(scr, &lv_font_montserrat_20, lv_color_white(), COL_X, 26, COL_W);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_track = make_label(scr, &lv_font_montserrat_14, COLOR_ACCENT, COL_X, 58, 0);

    /* Time line: position, "/ length", play state at the right end */
    s_time = make_label(scr, &lv_font_montserrat_28, lv_color_white(), COL_X, 82, 0);
    s_total = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X + 92, 94, 0);
    s_state = make_label(scr, &lv_font_montserrat_20, COLOR_ACCENT, 0, 90, 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_RIGHT, -COL_X, 90);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, COL_W, 8);
    lv_obj_set_pos(s_bar, COL_X, 124);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COLOR_ACCENT, LV_PART_INDICATOR);

    /* Bottom line: "Vol 100%   Buf  92%   (wifi) (warning)". Values sit in fixed-width, right-aligned
     * boxes so nothing moves when their digit count changes. */
    make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X, 144, 0);
    lv_label_set_text(lv_obj_get_child(scr, -1), "Vol");
    s_vol = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X + 24, 144, 42);
    lv_obj_set_style_text_align(s_vol, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X + 82, 144, 0);
    lv_label_set_text(lv_obj_get_child(scr, -1), "Buf");
    s_buf = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X + 106, 144, 42);
    lv_obj_set_style_text_align(s_buf, LV_TEXT_ALIGN_RIGHT, 0);
    s_wifi = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X + 164, 144, 0);
    s_warn = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X + 188, 144, 0);
    s_disc_len = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 0, 144, 0);
    lv_obj_align(s_disc_len, LV_ALIGN_TOP_RIGHT, -COL_X, 144);

    lv_timer_create(update_cb, 200, NULL);
}

esp_err_t ui_init(void)
{
    ESP_RETURN_ON_ERROR(tdisplay_s3_init(NULL), TAG, "display");
    if (lvgl_port_lock(0)) {
        build_screen();
        lvgl_port_unlock();
    }
    return ESP_OK;
}
