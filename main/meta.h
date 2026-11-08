/*
 * Album metadata and cover art from MusicBrainz / Cover Art Archive, cached in LittleFS
 *
 * When a disc is loaded, the cache (/data/meta/<discid>.json + .jpg) is checked first. CD-TEXT text wins over
 * the online data, so for CD-TEXT discs this mostly adds the cover.
 * Without a cache entry, the disc is looked up online as soon as Wi-Fi is connected. Results are handed
 * to the player (player_set_metadata()) and the cover to the UI.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "usb/atapi_host.h"
#include "discid.h"

#define META_COVER_SIZE 72  /* on-screen cover: META_COVER_SIZE² pixels, RGB565 */

typedef enum {
    META_IDLE,          /*!< No disc, or a disc with CD-TEXT (no lookup needed) */
    META_WAITING,       /*!< Lookup pending, waiting for Wi-Fi */
    META_LOOKING_UP,    /*!< Online lookup running */
    META_FROM_CACHE,    /*!< Metadata loaded from the cache */
    META_FOUND,         /*!< Metadata found online (and cached) */
    META_NOT_FOUND,     /*!< Disc unknown to MusicBrainz */
    META_FAILED,        /*!< Lookup failed (network / server); retried later */
} meta_state_t;

typedef struct {
    char discid[DISCID_LEN + 1]; /*!< Empty without a disc */
    meta_state_t state;
    bool has_cover;
    char release[40];            /*!< MusicBrainz release ID, empty if unknown */
    char error[96];              /*!< Reason of the last failed lookup, empty if none */
} meta_info_t;

esp_err_t meta_init(void);

/** A disc was loaded (after player_load_disc()): cache, then online lookup */
void meta_disc_loaded(const atapi_toc_t *toc, bool has_cdtext);
void meta_disc_unloaded(void);

/** Look up the current disc online again, even if cached or remembered as not found */
void meta_refresh(void);

/** Delete the current disc's cache entry (the shown data stays until the disc is reloaded) */
void meta_forget(void);

void meta_get_info(meta_info_t *info);
const char *meta_state_name(meta_state_t state);

/**
 * For the UI: if the cover changed since @p gen, copy it into @p buf (META_COVER_SIZE² RGB565), set
 * @p has_cover and update @p gen. Returns true if something changed.
 */
bool meta_get_cover(uint32_t *gen, uint16_t *buf, bool *has_cover);

/** Path of the current disc's cached cover JPEG; false if there is none */
bool meta_cover_path(char *path, size_t size);

void meta_print_info(void);
