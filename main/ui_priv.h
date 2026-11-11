/*
 * Shared UI helpers (ui.c) and the interface every screen layout implements (ui_layout_*.c)
 *
 * All functions run in the LVGL task (inside lvgl_port_lock()).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "player.h"
#include "ui_layout.h"

/* Montserrat with Latin-1 + Latin Extended-A + LVGL symbols (main/fonts, see font_conv.ps1) */
LV_FONT_DECLARE(font_text_14);
LV_FONT_DECLARE(font_text_20);
LV_FONT_DECLARE(font_text_28);
LV_FONT_DECLARE(font_text_40);

#define UI_COLOR_DIM    lv_color_hex(0x8a8a8a)
#define UI_COLOR_ACCENT lv_color_hex(0x37b4ff)

/* ---- implemented by the layout ---- */

/** Create the screen's widgets */
void ui_layout_build(lv_obj_t *scr);

/** Refresh from the player status (every 200 ms) */
void ui_layout_update(const player_status_t *st);

/* ---- helpers (ui.c) ---- */

/** Set a label's text, only if it changed (keeps scroll animations running) */
void ui_set_text(lv_obj_t *lbl, const char *text);

/** Same for UTF-8 text from the disc/metadata: characters the fonts can't show are replaced */
void ui_set_text_utf8(lv_obj_t *lbl, const char *utf8);

/** "mm:ss" from a sector count */
void ui_fmt_time(char *buf, size_t len, uint32_t sectors);

lv_obj_t *ui_make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int x, int y, int w);
void ui_set_hidden(lv_obj_t *obj, bool hidden);

/**
 * For click handlers: a press stays with the widget it started on (LV_OBJ_FLAG_PRESS_LOCK, so sliding onto a
 * neighbour doesn't move it there), and LVGL then reports a click wherever the finger is lifted. True only if it
 * was lifted on the widget (always true for keypad / encoder input).
 */
bool ui_released_inside(lv_event_t *e);

/* ---- settings / status menu (ui_menu.c) ---- */

/** Create the (hidden) menu over the player screen; call after ui_layout_build() */
void ui_menu_build(lv_obj_t *scr);
void ui_menu_open(void);
void ui_menu_close(void);
bool ui_menu_is_open(void);

/** Refresh the shown values (every 200 ms, from the UI timer) */
void ui_menu_update(void);

/** Every menu control is in this group: bind a keypad / encoder input device to it for button navigation */
lv_group_t *ui_menu_group(void);

/**
 * Cover art (UI_COVER_SIZE² RGB565). Call from ui_layout_update(): returns true when the image changed;
 * @p has_cover tells whether there is one. The descriptor stays the same, only its pixels change.
 */
bool ui_cover_poll(bool *has_cover);
const lv_image_dsc_t *ui_cover_image(void);
