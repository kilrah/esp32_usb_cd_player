/*
 * Small screen layout (320x170 class, no touch: T-Display S3)
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
 * lines then end left of it.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "ui_layout.h"
#if !UI_LAYOUT_LARGE

#include <stdio.h>
#include "esp_timer.h"
#include "net.h"
#include "ota.h"
#include "meta.h"
#include "ui_priv.h"

#define COL_X        10
#define COL_W        (BOARD_LCD_H_RES - 2 * COL_X)
#define COVER_X      (BOARD_LCD_H_RES - COL_X - UI_COVER_SIZE)
#define COVER_Y      6
#define TEXT_W_COVER (COVER_X - 8 - COL_X)   /* album/title width when the cover is shown */

static lv_obj_t *s_album, *s_title, *s_track, *s_state, *s_time, *s_total, *s_bar, *s_disc_len;
static lv_obj_t *s_vol, *s_buf, *s_wifi, *s_warn; /* bottom line: fixed slots, values right-aligned */
static lv_obj_t *s_cover, *s_spinner;

/** Cover image, and a spinner in its place (on top of an existing cover) while a lookup runs */
static void update_cover(void)
{
    bool has_cover;
    if (ui_cover_poll(&has_cover) && has_cover) {
        /* Same descriptor, new pixels: raw RGB565 is drawn from memory (no image cache), so a redraw is enough */
        lv_image_set_src(s_cover, ui_cover_image());
        lv_obj_invalidate(s_cover);
    }
    meta_info_t mi;
    meta_get_info(&mi);
    bool looking_up = mi.state == META_LOOKING_UP;
    ui_set_hidden(s_cover, !has_cover);
    ui_set_hidden(s_spinner, !looking_up);

    int32_t w = (has_cover || looking_up) ? TEXT_W_COVER : COL_W;
    if (lv_obj_get_width(s_album) != w) {
        lv_obj_set_width(s_album, w);
        lv_obj_set_width(s_title, w);
    }
}

void ui_layout_update(const player_status_t *st)
{
    char buf[2 * PLAYER_TEXT_LEN + 8];
    update_cover();

    if (st->state == PLAYER_NO_DISC) {
        ui_set_text(s_album, "");
        ui_set_text_utf8(s_title, st->message);
        ui_set_text(s_track, "");
        ui_set_text(s_state, "");
        ui_set_text(s_time, "--:--");
        ui_set_text(s_total, "");
        ui_set_text(s_disc_len, "");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    } else {
        if (st->artist[0] && st->album[0]) {
            snprintf(buf, sizeof(buf), "%s - %s", st->artist, st->album);
        } else {
            snprintf(buf, sizeof(buf), "%s", st->artist[0] ? st->artist : st->album[0] ? st->album : "Audio CD");
        }
        ui_set_text_utf8(s_album, buf);

        if (st->performer[0]) {
            snprintf(buf, sizeof(buf), "%s - %s", st->title, st->performer);
            ui_set_text_utf8(s_title, buf);
        } else {
            ui_set_text_utf8(s_title, st->title);
        }

        snprintf(buf, sizeof(buf), "Track %d / %d%s", st->track, st->num_tracks, st->deemphasis ? "   De-emph." : "");
        ui_set_text(s_track, buf);
        ui_set_text(s_state, st->state == PLAYER_PLAYING ? LV_SYMBOL_PLAY :
                    st->state == PLAYER_PAUSED ? LV_SYMBOL_PAUSE : LV_SYMBOL_STOP);

        ui_fmt_time(buf, sizeof(buf), st->track_pos);
        ui_set_text(s_time, buf);
        buf[0] = '/';
        buf[1] = ' ';
        ui_fmt_time(buf + 2, sizeof(buf) - 2, st->track_len);
        ui_set_text(s_total, buf);
        ui_fmt_time(buf, sizeof(buf), st->disc_len);
        ui_set_text(s_disc_len, buf);
        lv_bar_set_value(s_bar, st->track_len ? (int32_t)((uint64_t)st->track_pos * 1000 / st->track_len) : 0,
                         LV_ANIM_OFF);
    }

    int ota = ota_progress();
    if (ota >= 0) {
        snprintf(buf, sizeof(buf), "FW update: %d%%", ota);
        ui_set_text(s_title, buf);
    }

    net_status_t net;
    net_get_status(&net);
    if (net.ap_active && !net.connected && !net.eth_connected && st->state != PLAYER_PLAYING &&
        st->state != PLAYER_PAUSED) {
        /* Setup access point: how to reach the web UI. Network + password and the address alternate every
         * 3 s: together they don't fit the line (220 px next to the cover) and would scroll.
         * Only when idle (stopped or no disc); during playback the line shows the album. */
        if ((esp_timer_get_time() / 3000000) % 2 == 0) {
            const char *pw = net_ap_password();
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s / %s", net.ap_ssid, pw[0] ? pw : "open");
        } else {
            snprintf(buf, sizeof(buf), "Setup: http://" NET_AP_IP_STR);
        }
        ui_set_text(s_album, buf);
    }
    snprintf(buf, sizeof(buf), "%d%%", player_volume_display(st->volume));
    ui_set_text(s_vol, buf);
    snprintf(buf, sizeof(buf), "%d%%", st->buffer_pct);
    ui_set_text(s_buf, buf);
    ui_set_text(s_wifi, net.connected ? LV_SYMBOL_WIFI : "");
    ui_set_text(s_warn, st->read_errors ? LV_SYMBOL_WARNING : "");
}

void ui_layout_build(lv_obj_t *scr)
{
    /* Cover art, top right (hidden until there is one) */
    s_cover = lv_image_create(scr);
    lv_obj_set_pos(s_cover, COVER_X, COVER_Y);
    lv_obj_set_size(s_cover, UI_COVER_SIZE, UI_COVER_SIZE);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);

    /* Spinner in the cover area while album info is being looked up online */
    const int spin = 36;
    s_spinner = lv_spinner_create(scr);
    lv_spinner_set_anim_params(s_spinner, 1000, 240);
    lv_obj_set_size(s_spinner, spin, spin);
    lv_obj_set_pos(s_spinner, COVER_X + (UI_COVER_SIZE - spin) / 2, COVER_Y + (UI_COVER_SIZE - spin) / 2);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    /* Track info */
    s_album = ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X, 6, COL_W);
    lv_label_set_long_mode(s_album, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_title = ui_make_label(scr, &font_text_20, lv_color_white(), COL_X, 26, COL_W);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_track = ui_make_label(scr, &font_text_14, UI_COLOR_ACCENT, COL_X, 58, 0);

    /* Time line: position, "/ length", play state at the right end */
    s_time = ui_make_label(scr, &lv_font_montserrat_28, lv_color_white(), COL_X, 82, 0);
    s_total = ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X + 92, 94, 0);
    s_state = ui_make_label(scr, &font_text_20, UI_COLOR_ACCENT, 0, 90, 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_RIGHT, -COL_X, 90);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, COL_W, 8);
    lv_obj_set_pos(s_bar, COL_X, 124);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);

    /* Bottom line: "Vol 100%   Buf  92%   (wifi) (warning)". Values sit in fixed-width, right-aligned
     * boxes so nothing moves when their digit count changes. */
    lv_label_set_text(ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X, 144, 0), "Vol");
    s_vol = ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X + 24, 144, 42);
    lv_obj_set_style_text_align(s_vol, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X + 82, 144, 0), "Buf");
    s_buf = ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X + 106, 144, 42);
    lv_obj_set_style_text_align(s_buf, LV_TEXT_ALIGN_RIGHT, 0);
    s_wifi = ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X + 164, 144, 0);
    s_warn = ui_make_label(scr, &font_text_14, UI_COLOR_DIM, COL_X + 188, 144, 0);
    s_disc_len = ui_make_label(scr, &font_text_14, UI_COLOR_DIM, 0, 144, 0);
    lv_obj_align(s_disc_len, LV_ALIGN_TOP_RIGHT, -COL_X, 144);
}

#endif /* !UI_LAYOUT_LARGE */
