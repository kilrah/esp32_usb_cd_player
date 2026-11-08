/*
 * Application-level actions (cd_player_main.c) used by the other input paths (web UI)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

/** Open the tray, or close it if it is open (handled by the main task) */
void app_eject_toggle(void);
