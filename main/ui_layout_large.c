/*
 * Large screen layout (1024x600 class, touch: JC1060P470)
 *
 *  +--------------------------------------------------------------------------------------+
 *  | +-----------+  Artist                                                        [list] |
 *  | |           |  Album                                                                |
 *  | |   cover   |  Track title                                                   (40)   |
 *  | |  300x300  |  Track 3 / 11                                                         |
 *  | |           |  01:23 / 04:17                                                        |
 *  | +-----------+                                                                       |
 *  | [==================o---------------------------------------------------------------] |  seek by dragging
 *  | Buffer 92%  (wifi) (warn)                                                Disc 45:12 |
 *  |                                                                                      |
 *  | [eject] [prev] [play/pause] [stop] [next]                              70 %         |
 *  |                                                                  [======o------]     |
 *  +--------------------------------------------------------------------------------------+
 *
 * [list] opens the track list over the whole screen; tapping a track plays it.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "ui_layout.h"
#if UI_LAYOUT_LARGE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include "ota.h"
#include "meta.h"
#include "app.h"
#include "ui_priv.h"

#define M            24                                  /* outer margin */
#define COVER_X      M
#define COVER_Y      M
#define INFO_X       (COVER_X + UI_COVER_SIZE + 32)
#define INFO_W       (BOARD_LCD_H_RES - M - INFO_X)
#define LIST_BTN     64                                  /* track list button, top right */
#define SEEK_Y       (COVER_Y + UI_COVER_SIZE + 30)
#define STATUS_Y     (SEEK_Y + 30)
#define BTN_W        110
#define BTN_H        100
#define BTN_GAP      20
#define BTN_Y        (BOARD_LCD_V_RES - M - BTN_H)
#define VOL_X        (M + 4 * BTN_W + (BTN_W + 30) + 5 * BTN_GAP + 24)  /* right of the transport buttons */
#define VOL_W        (BOARD_LCD_H_RES - M - 12 - VOL_X)                /* slider; the knob overhangs 12 px */

#define COLOR_BTN       lv_color_hex(0x262626)
#define COLOR_BTN_PRESS lv_color_hex(0x3c3c3c)
#define COLOR_TRACK     lv_color_hex(0x303030)

static lv_obj_t *s_cover, *s_cover_ph, *s_spinner;
static lv_obj_t *s_artist, *s_album, *s_title, *s_track, *s_time, *s_total;
static lv_obj_t *s_seek, *s_buf, *s_wifi, *s_lan, *s_warn, *s_disc_len, *s_vol, *s_vol_slider;
static lv_obj_t *s_btn_play, *s_btn_play_lbl, *s_btn_stop, *s_btn_prev, *s_btn_next, *s_btn_list;
static lv_obj_t *s_list_panel, *s_list;
static uint32_t s_seek_len;     /* track length (sectors) when the seek slider was grabbed */

/* ------------------------------------------------------------------------ */
/* Widgets                                                                  */
/* ------------------------------------------------------------------------ */

static void style_button(lv_obj_t *b)
{
    lv_obj_set_style_bg_color(b, COLOR_BTN, 0);
    lv_obj_set_style_bg_color(b, COLOR_BTN_PRESS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(0x505050), LV_STATE_DISABLED);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *sym, const lv_font_t *font,
                             lv_event_cb_t cb, lv_event_code_t code, void *arg, lv_obj_t **label)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    style_button(b);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, code, arg);
    if (label) {
        *label = l;
    }
    return b;
}

static void set_enabled(lv_obj_t *obj, bool on)
{
    if (on == lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        if (on) {
            lv_obj_remove_state(obj, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(obj, LV_STATE_DISABLED);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Touch actions                                                            */
/* ------------------------------------------------------------------------ */

/**
 * A press stays with the widget it started on (LV_OBJ_FLAG_PRESS_LOCK, so sliding onto a neighbour doesn't move
 * it there), and LVGL then reports a click wherever the finger is lifted. Only act when it's lifted on the widget.
 */
static bool released_inside(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return true;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

static void cmd_cb(lv_event_t *e)
{
    if (released_inside(e)) {
        player_command((player_cmd_t)(intptr_t)lv_event_get_user_data(e), 0);
    }
}

static void eject_cb(lv_event_t *e)
{
    if (released_inside(e)) {
        app_eject_toggle();
    }
}

/** Volume slider (internal units 0..PLAYER_VOLUME_MAX): applied live while dragging */
static void volume_cb(lv_event_t *e)
{
    static int32_t last = -1;
    int32_t v = lv_slider_get_value(s_vol_slider);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        last = -1;
    }
    if (v != last) {
        last = v;
        player_command(PLAYER_CMD_VOLUME_SET, (int)v);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d %%", player_volume_display((int)v));
        ui_set_text(s_vol, buf);
    }
}

/** Seek slider: the time follows the finger, the jump happens on release */
static void seek_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    player_status_t st;
    char buf[16];
    if (code == LV_EVENT_PRESSED) {
        player_get_status(&st);
        s_seek_len = st.track_len;
    } else if (code == LV_EVENT_VALUE_CHANGED && lv_obj_has_state(s_seek, LV_STATE_PRESSED)) {
        ui_fmt_time(buf, sizeof(buf), (uint32_t)((uint64_t)lv_slider_get_value(s_seek) * s_seek_len / 1000));
        ui_set_text(s_time, buf);
    } else if (code == LV_EVENT_RELEASED && s_seek_len) {
        uint32_t s = (uint32_t)((uint64_t)lv_slider_get_value(s_seek) * s_seek_len / 1000 / ATAPI_CD_FRAMES_PER_SECOND);
        player_command(PLAYER_CMD_SEEK_ABS, (int)s);
        s_seek_len = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Track list (full screen, over the player)                                */
/* ------------------------------------------------------------------------ */

static void list_close(void)
{
    lv_obj_clean(s_list);
    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN);
}

static void list_close_cb(lv_event_t *e)
{
    if (released_inside(e)) {
        list_close();
    }
}

static void list_track_cb(lv_event_t *e)
{
    if (!released_inside(e)) {
        return;
    }
    player_command(PLAYER_CMD_TRACK, (int)(intptr_t)lv_event_get_user_data(e));
    list_close();
}

static void list_open_cb(lv_event_t *e)
{
    if (!released_inside(e)) {
        return;
    }
    player_track_info_t *tracks = malloc(ATAPI_MAX_TRACKS * sizeof(*tracks)); /* ~26 KB, PSRAM */
    if (!tracks) {
        return;
    }
    player_status_t st;
    player_get_status(&st);
    int n = player_get_tracks(tracks, ATAPI_MAX_TRACKS);
    if (n == 0) {
        free(tracks);
        return;
    }
    lv_obj_t *current = NULL;
    char text[2 * PLAYER_TEXT_LEN + 8], len[16];
    for (int i = 0; i < n; i++) {
        const player_track_info_t *t = &tracks[i];
        /* Row: number | title (wraps, the row grows with it) | length, vertically centred */
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 64, 0);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(row, t->number == st.track ? lv_color_hex(0x10324a) : lv_color_hex(0x141414), 0);
        lv_obj_set_style_bg_color(row, COLOR_BTN_PRESS, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        snprintf(text, sizeof(text), "%2d", t->number);
        lv_obj_t *num = ui_make_label(row, &font_text_28, t->number == st.track ? UI_COLOR_ACCENT : UI_COLOR_DIM, 0, 0, 56);
        lv_label_set_text(num, text);

        if (t->title[0] && t->performer[0]) {
            snprintf(text, sizeof(text), "%s - %s", t->title, t->performer);
        } else {
            snprintf(text, sizeof(text), "%s", t->title[0] ? t->title : t->audio ? "" : "(data)");
        }
        if (!text[0]) {
            snprintf(text, sizeof(text), "Track %d", t->number);
        }
        lv_obj_t *title = ui_make_label(row, &font_text_28, t->audio ? lv_color_white() : UI_COLOR_DIM, 0, 0, 0);
        lv_obj_set_flex_grow(title, 1);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
        ui_set_text_utf8(title, text);

        ui_fmt_time(len, sizeof(len), t->length);
        lv_obj_t *l = ui_make_label(row, &font_text_28, UI_COLOR_DIM, 0, 0, 0);
        lv_label_set_text(l, len);

        if (t->audio) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, list_track_cb, LV_EVENT_CLICKED, (void *)(intptr_t)t->number);
        }
        if (t->number == st.track) {
            current = row;
        }
    }
    free(tracks);
    lv_obj_remove_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_list);
    if (current) {
        lv_obj_scroll_to_view(current, LV_ANIM_OFF);
    }
}

static void build_list_panel(lv_obj_t *scr)
{
    s_list_panel = lv_obj_create(scr);
    lv_obj_set_pos(s_list_panel, 0, 0);
    lv_obj_set_size(s_list_panel, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(s_list_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_list_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list_panel, 0, 0);
    lv_obj_set_style_radius(s_list_panel, 0, 0);
    lv_obj_set_style_pad_all(s_list_panel, 0, 0);
    lv_obj_remove_flag(s_list_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = ui_make_label(s_list_panel, &font_text_28, UI_COLOR_DIM, M, M + 14, 0);
    lv_label_set_text(hdr, "Tracks");
    make_button(s_list_panel, BOARD_LCD_H_RES - M - LIST_BTN, M, LIST_BTN, LIST_BTN, LV_SYMBOL_CLOSE, &font_text_28,
                list_close_cb, LV_EVENT_CLICKED, NULL, NULL);

    s_list = lv_obj_create(s_list_panel);
    lv_obj_set_pos(s_list, M, M + LIST_BTN + 12);
    lv_obj_set_size(s_list, BOARD_LCD_H_RES - 2 * M, BOARD_LCD_V_RES - 2 * M - LIST_BTN - 12);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);

    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                  */
/* ------------------------------------------------------------------------ */

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
    ui_set_hidden(s_cover, !has_cover);
    ui_set_hidden(s_cover_ph, has_cover);
    ui_set_hidden(s_spinner, mi.state != META_LOOKING_UP);
}

void ui_layout_update(const player_status_t *st)
{
    char buf[2 * PLAYER_TEXT_LEN + 8];
    update_cover();
    bool disc = st->state != PLAYER_NO_DISC;
    bool seeking = lv_obj_has_state(s_seek, LV_STATE_PRESSED);

    if (!disc) {
        ui_set_text(s_artist, "");
        ui_set_text(s_album, "");
        ui_set_text_utf8(s_title, st->message);
        ui_set_text(s_track, "");
        ui_set_text(s_time, "--:--");
        ui_set_text(s_total, "");
        ui_set_text(s_disc_len, "");
        lv_slider_set_value(s_seek, 0, LV_ANIM_OFF);
        if (!lv_obj_has_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN)) {
            list_close(); /* disc gone */
        }
    } else {
        ui_set_text_utf8(s_artist, st->artist);
        ui_set_text_utf8(s_album, st->album[0] ? st->album : st->artist[0] ? "" : "Audio CD");
        if (st->performer[0]) {
            snprintf(buf, sizeof(buf), "%s - %s", st->title, st->performer);
            ui_set_text_utf8(s_title, buf);
        } else {
            ui_set_text_utf8(s_title, st->title);
        }
        snprintf(buf, sizeof(buf), "Track %d / %d%s", st->track, st->num_tracks, st->deemphasis ? "    De-emphasis" : "");
        ui_set_text(s_track, buf);

        if (!seeking) {
            ui_fmt_time(buf, sizeof(buf), st->track_pos);
            ui_set_text(s_time, buf);
            lv_slider_set_value(s_seek, st->track_len ? (int32_t)((uint64_t)st->track_pos * 1000 / st->track_len) : 0,
                                LV_ANIM_OFF);
        }
        buf[0] = '/';
        buf[1] = ' ';
        ui_fmt_time(buf + 2, sizeof(buf) - 2, st->track_len);
        ui_set_text(s_total, buf);
        strcpy(buf, "Disc ");
        ui_fmt_time(buf + 5, sizeof(buf) - 5, st->disc_len);
        ui_set_text(s_disc_len, buf);
    }

    int ota = ota_progress();
    if (ota >= 0) {
        snprintf(buf, sizeof(buf), "Firmware update: %d%%", ota);
        ui_set_text(s_title, buf);
    }

    net_status_t net;
    net_get_status(&net);
    if (net.ap_active && !net.connected && !net.eth_connected && st->state != PLAYER_PLAYING &&
        st->state != PLAYER_PAUSED) {
        /* Setup access point: how to reach the web UI (only when idle; during playback the lines show the album) */
        const char *pw = net_ap_password();
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Wi-Fi setup: join %s (password: %s)", net.ap_ssid,
                 pw[0] ? pw : "none");
        ui_set_text(s_artist, buf);
        ui_set_text(s_album, "then open http://" NET_AP_IP_STR);
    }

    /* Transport */
    bool playing = st->state == PLAYER_PLAYING;
    ui_set_text(s_btn_play_lbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    set_enabled(s_btn_play, disc);
    set_enabled(s_btn_stop, disc && st->state != PLAYER_STOPPED);
    set_enabled(s_btn_prev, disc);
    set_enabled(s_btn_next, disc && st->track < st->num_tracks);
    set_enabled(s_btn_list, disc);
    set_enabled(s_seek, disc && st->state != PLAYER_STOPPED);

    if (!lv_obj_has_state(s_vol_slider, LV_STATE_PRESSED)) { /* while dragging, the slider is the reference */
        lv_slider_set_value(s_vol_slider, st->volume, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d %%", player_volume_display(st->volume));
        ui_set_text(s_vol, buf);
    }
    snprintf(buf, sizeof(buf), "Buffer %d%%", st->buffer_pct);
    ui_set_text(s_buf, disc ? buf : "");
    ui_set_text(s_wifi, net.connected ? LV_SYMBOL_WIFI : "");
    ui_set_text(s_lan, net.eth_connected ? "LAN" : ""); /* no Ethernet symbol in LVGL */
    ui_set_text(s_warn, st->read_errors ? LV_SYMBOL_WARNING : "");
}

/* ------------------------------------------------------------------------ */

void ui_layout_build(lv_obj_t *scr)
{
    /* Cover art, a placeholder without one, and a spinner on top while album info is being looked up online */
    s_cover_ph = lv_obj_create(scr);
    lv_obj_set_pos(s_cover_ph, COVER_X, COVER_Y);
    lv_obj_set_size(s_cover_ph, UI_COVER_SIZE, UI_COVER_SIZE);
    lv_obj_set_style_bg_color(s_cover_ph, lv_color_hex(0x161616), 0);
    lv_obj_set_style_border_width(s_cover_ph, 0, 0);
    lv_obj_set_style_radius(s_cover_ph, 12, 0);
    lv_obj_remove_flag(s_cover_ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *note = ui_make_label(s_cover_ph, &font_text_40, lv_color_hex(0x404040), 0, 0, 0);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_center(note);

    s_cover = lv_image_create(scr);
    lv_obj_set_pos(s_cover, COVER_X, COVER_Y);
    lv_obj_set_size(s_cover, UI_COVER_SIZE, UI_COVER_SIZE);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);

    const int spin = 72;
    s_spinner = lv_spinner_create(scr);
    lv_spinner_set_anim_params(s_spinner, 1000, 240);
    lv_obj_set_size(s_spinner, spin, spin);
    lv_obj_set_pos(s_spinner, COVER_X + (UI_COVER_SIZE - spin) / 2, COVER_Y + (UI_COVER_SIZE - spin) / 2);
    lv_obj_set_style_arc_width(s_spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    /* Track info, right of the cover */
    const int text_w = INFO_W - LIST_BTN - 16;
    s_artist = ui_make_label(scr, &font_text_28, UI_COLOR_DIM, INFO_X, COVER_Y + 4, text_w);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_album = ui_make_label(scr, &font_text_28, lv_color_white(), INFO_X, COVER_Y + 46, text_w);
    lv_label_set_long_mode(s_album, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_title = ui_make_label(scr, &font_text_40, lv_color_white(), INFO_X, COVER_Y + 110, INFO_W);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_track = ui_make_label(scr, &font_text_28, UI_COLOR_ACCENT, INFO_X, COVER_Y + 170, INFO_W);
    s_time = ui_make_label(scr, &lv_font_montserrat_48, lv_color_white(), INFO_X, COVER_Y + 228, 0);
    s_total = ui_make_label(scr, &font_text_28, UI_COLOR_DIM, INFO_X + 152, COVER_Y + 244, 0);

    s_btn_list = make_button(scr, BOARD_LCD_H_RES - M - LIST_BTN, COVER_Y, LIST_BTN, LIST_BTN, LV_SYMBOL_LIST,
                             &font_text_28, list_open_cb, LV_EVENT_CLICKED, NULL, NULL);

    /* Seek slider: position in the track */
    s_seek = lv_slider_create(scr);
    lv_obj_set_pos(s_seek, M + 12, SEEK_Y);
    lv_obj_set_size(s_seek, BOARD_LCD_H_RES - 2 * M - 24, 12);
    lv_slider_set_range(s_seek, 0, 1000);
    lv_obj_set_style_bg_color(s_seek, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_seek, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_seek, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_seek, 8, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_seek, LV_OPA_TRANSP, LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(s_seek, 24); /* easier to grab */
    lv_obj_add_event_cb(s_seek, seek_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_seek, seek_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_seek, seek_cb, LV_EVENT_RELEASED, NULL);

    /* Status line */
    s_buf = ui_make_label(scr, &font_text_20, UI_COLOR_DIM, M, STATUS_Y, 150);
    s_wifi = ui_make_label(scr, &font_text_20, UI_COLOR_DIM, M + 160, STATUS_Y, 0);
    s_lan = ui_make_label(scr, &font_text_20, UI_COLOR_DIM, M + 196, STATUS_Y, 0);
    s_warn = ui_make_label(scr, &font_text_20, lv_color_hex(0xe0a020), M + 250, STATUS_Y, 0);
    s_disc_len = ui_make_label(scr, &font_text_20, UI_COLOR_DIM, 0, STATUS_Y, 0);
    lv_obj_align(s_disc_len, LV_ALIGN_TOP_RIGHT, -M, STATUS_Y);

    /* Transport buttons */
    int x = M;
    make_button(scr, x, BTN_Y, BTN_W, BTN_H, LV_SYMBOL_EJECT, &font_text_40, eject_cb, LV_EVENT_CLICKED, NULL, NULL);
    x += BTN_W + BTN_GAP;
    s_btn_prev = make_button(scr, x, BTN_Y, BTN_W, BTN_H, LV_SYMBOL_PREV, &font_text_40, cmd_cb, LV_EVENT_CLICKED,
                             (void *)(intptr_t)PLAYER_CMD_PREV, NULL);
    x += BTN_W + BTN_GAP;
    s_btn_play = make_button(scr, x, BTN_Y, BTN_W + 30, BTN_H, LV_SYMBOL_PLAY, &font_text_40, cmd_cb, LV_EVENT_CLICKED,
                             (void *)(intptr_t)PLAYER_CMD_PLAY_PAUSE, &s_btn_play_lbl);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(0x1b5a85), 0);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(0x2a78ad), LV_STATE_PRESSED);
    x += BTN_W + 30 + BTN_GAP;
    s_btn_stop = make_button(scr, x, BTN_Y, BTN_W, BTN_H, LV_SYMBOL_STOP, &font_text_40, cmd_cb, LV_EVENT_CLICKED,
                             (void *)(intptr_t)PLAYER_CMD_STOP, NULL);
    x += BTN_W + BTN_GAP;
    s_btn_next = make_button(scr, x, BTN_Y, BTN_W, BTN_H, LV_SYMBOL_NEXT, &font_text_40, cmd_cb, LV_EVENT_CLICKED,
                             (void *)(intptr_t)PLAYER_CMD_NEXT, NULL);

    /* Volume: slider (live), the percentage centred above it */
    s_vol = ui_make_label(scr, &font_text_28, lv_color_white(), VOL_X, BTN_Y + 8, VOL_W);
    lv_obj_set_style_text_align(s_vol, LV_TEXT_ALIGN_CENTER, 0);
    s_vol_slider = lv_slider_create(scr);
    lv_obj_set_pos(s_vol_slider, VOL_X, BTN_Y + 62);
    lv_obj_set_size(s_vol_slider, VOL_W, 12);
    lv_slider_set_range(s_vol_slider, 0, PLAYER_VOLUME_MAX);
    lv_obj_set_style_bg_color(s_vol_slider, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_vol_slider, 30); /* easier to grab */
    lv_obj_add_event_cb(s_vol_slider, volume_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_vol_slider, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_list_panel(scr);
}

#endif /* UI_LAYOUT_LARGE */
