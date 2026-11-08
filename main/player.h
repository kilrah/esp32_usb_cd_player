/*
 * CD audio player: streams CD-DA from the drive to I2S
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "usb/atapi_host.h"

typedef enum {
    PLAYER_NO_DISC,
    PLAYER_STOPPED,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

typedef enum {
    PLAYER_CMD_PLAY_PAUSE,
    PLAYER_CMD_PLAY,
    PLAYER_CMD_PAUSE,
    PLAYER_CMD_STOP,
    PLAYER_CMD_NEXT,
    PLAYER_CMD_PREV,        /*!< Restart track if > 3 s in, otherwise previous track */
    PLAYER_CMD_TRACK,       /*!< arg = track number */
    PLAYER_CMD_SEEK_REL,    /*!< arg = seconds (+/-) */
    PLAYER_CMD_VOLUME_REL,  /*!< arg = percent (+/-), clamped to 0..PLAYER_VOLUME_MAX */
    PLAYER_CMD_VOLUME_SET,  /*!< arg = internal volume 0..PLAYER_VOLUME_MAX */
    PLAYER_CMD_SEEK_ABS,    /*!< arg = seconds from the start of the current track */
} player_cmd_t;

/* Digital volume (attenuation only). Capped because the PCM5102 output (~2.1 Vrms at full scale)
 * overdrives the downstream amplifier. Gain = (volume/100)^2, so 50 % = -12 dB.
 * The user sees the volume scaled so that PLAYER_VOLUME_MAX = 100 % (see player_volume_display()). */
#define PLAYER_VOLUME_MAX   50
#define PLAYER_VOLUME_STEP  1   /* internal units; 2 % on the display with MAX = 50 */

/** Volume as shown to the user: 0..100 %, where 100 % = PLAYER_VOLUME_MAX */
static inline int player_volume_display(int volume)
{
    return (volume * 100 + PLAYER_VOLUME_MAX / 2) / PLAYER_VOLUME_MAX;
}

#define PLAYER_TEXT_LEN 128  /* bytes of UTF-8: accented text takes 2-3 bytes per character */

/** Snapshot of the player state for the UI (all strings UTF-8, never NULL) */
typedef struct {
    player_state_t state;
    int num_tracks;
    int track;               /*!< Current track number, 0 if none */
    uint32_t track_pos;      /*!< Sectors since track start */
    uint32_t track_len;      /*!< Track length in sectors */
    uint32_t disc_len;       /*!< Total disc length in sectors */
    int volume;              /*!< Internal 0..PLAYER_VOLUME_MAX; show via player_volume_display() */
    int buffer_pct;          /*!< Read-ahead buffer fill (sectors not yet sent to I2S) */
    uint32_t buffer_ms;      /*!< Read-ahead buffer in milliseconds of audio */
    uint32_t underruns;
    uint32_t read_errors;
    char album[PLAYER_TEXT_LEN];
    char artist[PLAYER_TEXT_LEN];
    char title[PLAYER_TEXT_LEN];     /*!< Current track title (or "Track N") */
    char performer[PLAYER_TEXT_LEN]; /*!< Current track performer, empty if same as album artist */
    char message[PLAYER_TEXT_LEN];   /*!< Drive/disc status message when there is no disc */
    bool preemphasis;        /*!< Current track is flagged with pre-emphasis */
    bool deemphasis;         /*!< De-emphasis filter active (pre-emphasis track, or forced) */
} player_status_t;

esp_err_t player_init(void);

/** Set the drive used for reading (NULL when the drive is gone). Waits for a running read to finish. */
void player_set_device(atapi_host_device_handle_t dev);

/**
 * Load a disc. The player copies @p toc and takes ownership of @p cdtext
 * (heap-allocated, may be NULL); it is freed by player_unload_disc().
 */
void player_load_disc(const atapi_toc_t *toc, atapi_cd_text_t *cdtext);
void player_unload_disc(void);

/** Status text shown when there is no disc (e.g. "Waiting for drive") */
void player_set_message(const char *msg);

void player_command(player_cmd_t cmd, int arg);

/** The drive motor was stopped for power saving (TEST UNIT READY may say "not ready" meanwhile) */
bool player_is_spun_down(void);

/** Apply the de-emphasis filter to all tracks, flagged or not (to audition / test it; not persisted) */
void player_force_deemphasis(bool on);
bool player_deemphasis_forced(void);
void player_get_status(player_status_t *st);

/** Album metadata from another source than CD-TEXT (online lookup / cache), indexed by track number */
typedef struct {
    char album[PLAYER_TEXT_LEN];
    char artist[PLAYER_TEXT_LEN];
    struct {
        char title[PLAYER_TEXT_LEN];
        char artist[PLAYER_TEXT_LEN];
    } track[ATAPI_MAX_TRACKS + 1];
} player_meta_t;

/**
 * Attach metadata to the loaded disc (takes ownership, NULL clears). CD-TEXT fields take precedence;
 * this fills the empty ones. Freed on unload / next disc.
 */
void player_set_metadata(player_meta_t *meta);

/** Track list entry (strings UTF-8, never NULL) */
typedef struct {
    int number;
    bool audio;
    uint32_t length;                 /*!< sectors */
    char title[PLAYER_TEXT_LEN];     /*!< CD-TEXT title, empty if none */
    char performer[PLAYER_TEXT_LEN]; /*!< CD-TEXT performer if different from the album artist, else empty */
} player_track_info_t;

/** Copy up to @p max tracks of the loaded disc. Returns the number of tracks (0 without a disc). */
int player_get_tracks(player_track_info_t *out, int max);

/** Start playing as soon as a disc is loaded (persisted in NVS, default on) */
bool player_get_autoplay(void);
void player_set_autoplay(bool on);
