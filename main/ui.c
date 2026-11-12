/*
 * CD player screen: shared part (display setup, refresh timer, backlight policy, cover image, text helpers)
 *
 * The widgets are created and updated by the layout for the display size (ui_layout.h). The text fonts cover
 * Latin-1 and Latin Extended-A; other characters show as '?'.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "player.h"
#include "net.h"
#include "ota.h"
#include "meta.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "ui";

/* Backlight settings: NVS namespace "display", key "backlight" (blob, versioned) */
#define BL_NVS_NAMESPACE "display"
#define BL_NVS_KEY       "backlight"
#define BL_CFG_VERSION   1

typedef struct {
    uint8_t version;
    uint8_t mode, on_level, dim_level;
    uint16_t dim_delay_s, off_delay_s;
} bl_nvs_t;

static portMUX_TYPE s_bl_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_backlight_cfg_t s_bl = {
    .mode = UI_DIM_IDLE, .on_level = BOARD_BRIGHTNESS_MAX, .dim_level = 4, .dim_delay_s = 30, .off_delay_s = 600,
};
static volatile bool s_bl_wake; /* settings changed: treat as activity */

static uint16_t *s_cover_px;                     /* RGB565, PSRAM; only touched in the LVGL task */
static lv_image_dsc_t s_cover_dsc;
static uint32_t s_cover_gen = UINT32_MAX;
static bool s_has_cover;

#if BOARD_HAS_TOUCH
static volatile bool s_touched; /* a touch counts as activity (wakes the backlight) */
#endif

/* ------------------------------------------------------------------------ */
/* UTF-8 filter for the display fonts                                       */
/* ------------------------------------------------------------------------ */

/*
 * The text fonts (main/fonts, generated with lv_font_conv, see font_conv.ps1) contain ASCII, Latin-1 Supplement,
 * Latin Extended-A, the Cyrillic block U+0400-U+04FF, common typographic punctuation and the LVGL symbols.
 * Characters outside these would be drawn as nothing, so they are replaced.
 */
static bool in_font(uint32_t cp)
{
    return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0x17F) || (cp >= 0x400 && cp <= 0x4FF) ||
           (cp >= 0x2010 && cp <= 0x2015) || (cp >= 0x2018 && cp <= 0x201E) ||
           (cp >= 0x2020 && cp <= 0x2022) || cp == 0x2026 || cp == 0x2030 ||
           (cp >= 0x2032 && cp <= 0x2033) || (cp >= 0x2039 && cp <= 0x203A) ||
           cp == 0x20AC || cp == 0x2122 ||
           (cp >= 0xF000 && cp <= 0xF8FF); /* LV_SYMBOL_* (FontAwesome, private use area) */
}

static const char *replacement_for(uint32_t cp)
{
    switch (cp) {
    case 0x2212: return "-";   /* minus sign */
    case 0x2027: return "-";
    case 0x2044: return "/";   /* fraction slash */
    case 0x0009: return " ";   /* tab */
    default: return "?";
    }
}

/** Copy UTF-8 text, replacing what the fonts can't show (and broken sequences) */
static void to_display(char *dst, size_t size, const char *src)
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
            cp = 0xFFFD; /* stray byte */
            n = 1;
        }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { /* truncated sequence */
                cp = 0xFFFD;
                n = i;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (in_font(cp)) {
            if (o + n >= size) {
                break; /* don't cut a character in half */
            }
            memcpy(dst + o, p, n);
            o += n;
        } else {
            for (const char *rep = replacement_for(cp); *rep && o + 1 < size; rep++) {
                dst[o++] = *rep;
            }
        }
        p += n;
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Helpers for the layouts                                                  */
/* ------------------------------------------------------------------------ */

void ui_set_text(lv_obj_t *lbl, const char *text)
{
    /* Avoid resetting scroll animations when nothing changed */
    if (strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

void ui_set_text_utf8(lv_obj_t *lbl, const char *utf8)
{
    char buf[2 * PLAYER_TEXT_LEN + 8];
    to_display(buf, sizeof(buf), utf8);
    ui_set_text(lbl, buf);
}

void ui_fmt_time(char *buf, size_t len, uint32_t sectors)
{
    uint32_t s = sectors / ATAPI_CD_FRAMES_PER_SECOND;
    snprintf(buf, len, "%02"PRIu32":%02"PRIu32, s / 60, s % 60);
}

lv_obj_t *ui_make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int x, int y, int w)
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

void ui_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

bool ui_released_inside(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return true; /* keypad / encoder: ENTER on the focused widget */
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

bool ui_cover_poll(bool *has_cover)
{
    bool changed = s_cover_px && meta_get_cover(&s_cover_gen, s_cover_px, &s_has_cover);
    *has_cover = s_has_cover;
    return changed;
}

const lv_image_dsc_t *ui_cover_image(void)
{
    return &s_cover_dsc;
}

/* ------------------------------------------------------------------------ */
/* Backlight                                                                */
/* ------------------------------------------------------------------------ */

static void clamp_backlight(ui_backlight_cfg_t *c)
{
    c->mode = c->mode > UI_DIM_ALWAYS ? UI_DIM_IDLE : c->mode;
    c->on_level = c->on_level < 1 ? 1 : c->on_level > BOARD_BRIGHTNESS_MAX ? BOARD_BRIGHTNESS_MAX : c->on_level;
    c->dim_level = c->dim_level < 0 ? 0 : c->dim_level > c->on_level ? c->on_level : c->dim_level;
    c->dim_delay_s = c->dim_delay_s < 5 ? 5 : c->dim_delay_s > 3600 ? 3600 : c->dim_delay_s;
    c->off_delay_s = c->off_delay_s < 0 ? 0 : c->off_delay_s > 24 * 3600 ? 24 * 3600 : c->off_delay_s;
}

static void load_backlight(void)
{
    nvs_handle_t h;
    bl_nvs_t v;
    size_t len = sizeof(v);
    if (nvs_open(BL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, BL_NVS_KEY, &v, &len) == ESP_OK && len == sizeof(v) && v.version == BL_CFG_VERSION) {
        ui_backlight_cfg_t c = {.mode = v.mode, .on_level = v.on_level, .dim_level = v.dim_level,
                                .dim_delay_s = v.dim_delay_s, .off_delay_s = v.off_delay_s};
        clamp_backlight(&c);
        s_bl = c;
    }
    nvs_close(h);
}

const char *ui_dim_mode_name(ui_dim_mode_t mode)
{
    static const char *names[] = {"never", "idle", "always"};
    return mode <= UI_DIM_ALWAYS ? names[mode] : "?";
}

void ui_get_backlight(ui_backlight_cfg_t *cfg)
{
    portENTER_CRITICAL(&s_bl_lock);
    *cfg = s_bl;
    portEXIT_CRITICAL(&s_bl_lock);
}

esp_err_t ui_set_backlight(const ui_backlight_cfg_t *cfg)
{
    ui_backlight_cfg_t c = *cfg;
    clamp_backlight(&c);
    portENTER_CRITICAL(&s_bl_lock);
    s_bl = c;
    portEXIT_CRITICAL(&s_bl_lock);
    s_bl_wake = true;
    bl_nvs_t v = {.version = BL_CFG_VERSION, .mode = c.mode, .on_level = c.on_level, .dim_level = c.dim_level,
                  .dim_delay_s = c.dim_delay_s, .off_delay_s = c.off_delay_s};
    nvs_handle_t h;
    esp_err_t err = nvs_open(BL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, BL_NVS_KEY, &v, sizeof(v));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Backlight: dim %s, on %d, dim %d after %d s, off after %d s idle%s", ui_dim_mode_name(c.mode),
             c.on_level, c.dim_level, c.dim_delay_s, c.off_delay_s, err == ESP_OK ? "" : " (NOT saved)");
    return err;
}

/**
 * Activity = anything the user would notice or cause: play state, track, volume, disc, status message,
 * firmware update, setup AP, touching the screen. Playback position changes don't count.
 */
static void update_backlight(const player_status_t *st)
{
    static uint32_t last_sig;
    static int64_t last_activity;
    net_status_t net;
    net_get_status(&net);
    uint32_t sig = st->state | st->track << 4 | st->volume << 12 | (uint32_t)(ota_progress() >= 0) << 20 |
                   (uint32_t)net.ap_active << 21;
    for (const char *c = st->message; *c; c++) {
        sig = sig * 31 + (unsigned char)*c;
    }
    sig ^= st->disc_len * 2654435761u;
    int64_t now = esp_timer_get_time() / 1000000;
    bool touched = false;
#if BOARD_HAS_TOUCH
    touched = s_touched;
    s_touched = false;
#endif
    if (sig != last_sig || last_activity == 0 || s_bl_wake || touched) {
        s_bl_wake = false;
        last_sig = sig;
        last_activity = now;
    }
    ui_backlight_cfg_t c;
    ui_get_backlight(&c);
    bool playing = st->state == PLAYER_PLAYING;
    int64_t quiet = now - last_activity; /* seconds without activity */
    bool dim = c.mode == UI_DIM_ALWAYS || (c.mode == UI_DIM_IDLE && !playing);
    int level = c.on_level;
    if (!playing && c.off_delay_s && quiet >= c.off_delay_s) {
        level = 0;
    } else if (dim && quiet >= c.dim_delay_s) {
        level = c.dim_level;
    }
    if (level != board_get_brightness()) {
        board_set_brightness(level);
    }
}

#if BOARD_HAS_TOUCH
/**
 * Any press on the screen brightens the backlight. While it is dimmed, the press also acts normally (the screen
 * is readable). While it is off, the first touch only wakes it up and doesn't reach the widget: pressing
 * "stop" on a dark screen shouldn't stop the music.
 */
static void touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_target(e); /* indev events are sent before the widget gets the press */
    s_touched = true;
    ui_backlight_cfg_t c;
    ui_get_backlight(&c);
    int level = board_get_brightness();
    if (level < c.on_level) {
        board_set_brightness(c.on_level);
        if (level == 0) {
            lv_indev_reset(indev, NULL);      /* the press doesn't reach the widget */
            lv_indev_wait_release(indev);     /* nor does the rest of this touch */
        }
    }
}
#endif

/* ------------------------------------------------------------------------ */

static void update_cb(lv_timer_t *t)
{
    player_status_t st;
    player_get_status(&st);
    update_backlight(&st);
    ui_layout_update(&st);
    ui_menu_update();
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_cover_px = heap_caps_calloc(1, UI_COVER_SIZE * UI_COVER_SIZE * 2, MALLOC_CAP_SPIRAM);
    s_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_cover_dsc.header.w = UI_COVER_SIZE;
    s_cover_dsc.header.h = UI_COVER_SIZE;
    s_cover_dsc.header.stride = UI_COVER_SIZE * 2;
    s_cover_dsc.data_size = UI_COVER_SIZE * UI_COVER_SIZE * 2;
    s_cover_dsc.data = (const uint8_t *)s_cover_px;

    ui_layout_build(scr);
    ui_menu_build(scr); /* on top of the layout */
    lv_timer_create(update_cb, 200, NULL);
}

esp_err_t ui_init(void)
{
    load_backlight();
    lv_display_t *disp = NULL;
    ESP_RETURN_ON_ERROR(board_display_init(&disp), TAG, "display");
    if (lvgl_port_lock(0)) {
        build_screen();
#if BOARD_HAS_TOUCH
        for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
            if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
                lv_indev_add_event_cb(in, touch_cb, LV_EVENT_PRESSED, NULL);
            }
        }
#endif
        lvgl_port_unlock();
    }
    return ESP_OK;
}
