/*
 * Screen layouts: chosen by the display size, not by the board, so a new board with a known resolution only needs
 * its board component.
 *
 *  ui_layout_small.c  320x170 class, no touch (T-Display S3)
 *  ui_layout_large.c  1024x600 class, touch transport controls (JC1060P470)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "board.h"

#if BOARD_LCD_H_RES >= 800 && BOARD_LCD_V_RES >= 480
#define UI_LAYOUT_LARGE 1
#define UI_COVER_SIZE   300   /* px, square */
#else
#define UI_LAYOUT_LARGE 0
#define UI_COVER_SIZE   72
#endif
