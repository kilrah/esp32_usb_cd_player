/*
 * CD player screen (LVGL, 320x170 landscape)
 *
 *  +------------------------------------------------+
 *  | Artist - Album                            (14) |
 *  | Track title                               (20) |
 *  | Track 3 / 11                                > |
 *  | 01:23                  / 04:17                 |
 *  | [=============-------------------------------] |
 *  | Vol 100%  Buf 92%                       34:56 |
 *  +------------------------------------------------+
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "tdisplay_s3.h"
#include "player.h"
#include "ui.h"

static const char *TAG = "ui";

#define COL_X      10
#define COL_W      (TDS3_LCD_H_RES - 2 * COL_X)
#define COLOR_DIM    lv_color_hex(0x8a8a8a)
#define COLOR_ACCENT lv_color_hex(0x37b4ff)

static lv_obj_t *s_album, *s_title, *s_track, *s_state, *s_time, *s_total, *s_bar, *s_info, *s_disc_len;

static void set_text(lv_obj_t *lbl, const char *text)
{
    /* Avoid resetting scroll animations when nothing changed */
    if (strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
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

static void update_cb(lv_timer_t *t)
{
    player_status_t st;
    char buf[2 * PLAYER_TEXT_LEN + 8];
    player_get_status(&st);

    if (st.state == PLAYER_NO_DISC) {
        set_text(s_album, "");
        set_text(s_title, st.message);
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
        set_text(s_album, buf);

        if (st.performer[0]) {
            snprintf(buf, sizeof(buf), "%s - %s", st.title, st.performer);
            set_text(s_title, buf);
        } else {
            set_text(s_title, st.title);
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

    snprintf(buf, sizeof(buf), "Vol %d%%   Buf %d%%%s", player_volume_display(st.volume), st.buffer_pct,
             st.read_errors ? "   " LV_SYMBOL_WARNING : "");
    set_text(s_info, buf);
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Track info */
    s_album = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X, 6, COL_W);
    lv_label_set_long_mode(s_album, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_title = make_label(scr, &lv_font_montserrat_20, lv_color_white(), COL_X, 26, COL_W);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    s_track = make_label(scr, &lv_font_montserrat_14, COLOR_ACCENT, COL_X, 58, 0);
    s_state = make_label(scr, &lv_font_montserrat_20, COLOR_ACCENT, 0, 54, 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_RIGHT, -COL_X, 54);

    s_time = make_label(scr, &lv_font_montserrat_28, lv_color_white(), COL_X, 82, 0);
    s_total = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X + 92, 94, 0);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, COL_W, 8);
    lv_obj_set_pos(s_bar, COL_X, 124);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COLOR_ACCENT, LV_PART_INDICATOR);

    s_info = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, COL_X, 144, 0);
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
