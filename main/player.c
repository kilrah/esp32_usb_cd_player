/*
 * CD audio player: streams CD-DA from the drive to I2S
 *
 * Reader task: reads chunks of sectors from the drive into a pool of PSRAM
 * buffers (read-ahead). Writer task: sends filled chunks to I2S, applies the
 * volume and tracks the play position. A generation counter invalidates all
 * buffered chunks on seek / track change / stop.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "player.h"
#include "audio_out.h"

static const char *TAG = "player";

#define CHUNK_SECTORS   16                                  /* one READ CD command */
#define CHUNK_BYTES     (CHUNK_SECTORS * ATAPI_CD_AUDIO_SECTOR_SIZE)
#define NUM_CHUNKS      40                                  /* ~8 s read-ahead, 1500 KB PSRAM: slipping discs
                                                               recover slowly after a full spin-down */
#define SLICE_SECTORS   2                                   /* I2S write granularity (~27 ms) */
#define READ_RETRIES    3
#define PREV_RESTART_SECTORS (3 * ATAPI_CD_FRAMES_PER_SECOND)
#define SLOW_READ_MS    500                                 /* log reads slower than this */
#define READ_SPEED_KBPS (4 * ATAPI_SPEED_1X_KBPS)          /* 4x: plenty of margin, less noise than max */
#define SPINDOWN_S      120                                 /* stop the drive motor after this long without playback */
#define NVS_NAMESPACE   "player"
#define NVS_KEY_VOLUME  "volume"
#define NVS_KEY_AUTOPLAY "autoplay"
#define VOLUME_SAVE_DELAY_MS 2000                           /* debounce NVS writes */

typedef struct {
    uint8_t *data;
    uint32_t lba;
    uint32_t count;
    uint32_t gen;
} chunk_t;

static struct {
    SemaphoreHandle_t lock;      /* protects everything below */
    SemaphoreHandle_t dev_lock;  /* held by the reader while using dev */
    QueueHandle_t free_q;
    QueueHandle_t filled_q;
    TaskHandle_t reader_task;
    TaskHandle_t writer_task;
    chunk_t chunks[NUM_CHUNKS];

    atapi_host_device_handle_t dev;
    player_state_t state;
    bool has_disc;
    atapi_toc_t toc;
    atapi_cd_text_t *cdtext;
    player_meta_t *meta;    /* online metadata (heap, PSRAM), fills what CD-TEXT lacks */
    char message[PLAYER_TEXT_LEN];

    uint32_t gen;
    uint32_t read_lba;      /* next sector to read */
    uint32_t play_lba;      /* sector currently being played */
    bool reader_done;       /* reached the end of the playable area */
    uint16_t speed_kbps;    /* read speed limit, 0 = drive maximum */
    uint8_t speed_methods;  /* PLAYER_SPEED_* bits: which commands set it */
    bool spun_down;         /* drive motor stopped by us (power saving); spun up again before reading */
    int64_t idle_since;     /* esp_timer time playback stopped/paused, 0 while playing */
    bool primed;            /* audio has been played since the last seek (for underrun counting) */
    uint32_t buffered;      /* sectors read ahead and not yet sent to I2S (current generation) */
    esp_timer_handle_t volume_save_timer;
    int volume;
    bool force_deemph;      /* apply de-emphasis to every track (test / audition) */
    bool autoplay;
    uint32_t underruns;
    uint32_t read_errors;
} P;

#define LOCK()   xSemaphoreTake(P.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(P.lock)

/* ------------------------------------------------------------------------ */
/* TOC helpers (call with lock held)                                        */
/* ------------------------------------------------------------------------ */

static int track_index_for_lba(uint32_t lba)
{
    for (int i = P.toc.num_tracks - 1; i >= 0; i--) {
        if (lba >= P.toc.tracks[i].lba) {
            return i;
        }
    }
    return 0;
}

static bool track_has_preemphasis(uint32_t lba)
{
    return P.toc.num_tracks > 0 && atapi_toc_entry_has_preemphasis(&P.toc.tracks[track_index_for_lba(lba)]);
}

static bool track_is_audio(int idx)
{
    return idx >= 0 && idx < P.toc.num_tracks && !atapi_toc_entry_is_data(&P.toc.tracks[idx]);
}

static int first_audio_track(int from_idx)
{
    for (int i = MAX(from_idx, 0); i < P.toc.num_tracks; i++) {
        if (track_is_audio(i)) {
            return i;
        }
    }
    return -1;
}

/** If @p lba is in a data track, move it to the next audio track (or the lead-out) */
static uint32_t skip_data_tracks(uint32_t lba)
{
    if (lba >= P.toc.leadout_lba) {
        return P.toc.leadout_lba;
    }
    int idx = track_index_for_lba(lba);
    if (track_is_audio(idx)) {
        return lba;
    }
    int next = first_audio_track(idx + 1);
    return next >= 0 ? P.toc.tracks[next].lba : P.toc.leadout_lba;
}

/** End of the contiguous audio area starting at @p lba (next data track or lead-out) */
static uint32_t audio_end_from(uint32_t lba)
{
    for (int i = track_index_for_lba(lba) + 1; i < P.toc.num_tracks; i++) {
        if (!track_is_audio(i)) {
            return P.toc.tracks[i].lba;
        }
    }
    return P.toc.leadout_lba;
}

/** Return buffered (stale after a gen change) chunks to the pool (call with lock held) */
static void drain_locked(void)
{
    chunk_t *c;
    while (xQueueReceive(P.filled_q, &c, 0) == pdTRUE) {
        xQueueSend(P.free_q, &c, 0);
    }
    P.buffered = 0; /* the writer's current chunk (if any) is stale as well */
}

/** Restart streaming at @p lba (call with lock held) */
static void seek_locked(uint32_t lba)
{
    lba = skip_data_tracks(lba);
    P.gen++;
    P.read_lba = lba;
    P.play_lba = lba;
    P.reader_done = false;
    P.primed = false;
    drain_locked();
    xTaskNotifyGive(P.reader_task);
    xTaskNotifyGive(P.writer_task);
}

/* ------------------------------------------------------------------------ */
/* Reader                                                                   */
/* ------------------------------------------------------------------------ */

/** Spin the drive down after SPINDOWN_S without playback, and up again before reading (reader task) */
/** Limit the read speed (call with dev_lock held). Drives differ in which command they honour: SET CD SPEED
 *  (older drives) and/or SET STREAMING (MMC-3+); P.speed_methods selects them (test command "speed").
 *  Some drives forget the setting after a spin-down, so this is repeated after each spin-up. */
static void apply_read_speed(void)
{
    uint32_t end = P.toc.leadout_lba ? P.toc.leadout_lba - 1 : 0;
    uint16_t kbps = P.speed_kbps ? P.speed_kbps : ATAPI_SPEED_MAX;
    const char *r1 = "-", *r2 = "-";
    if (P.speed_methods & PLAYER_SPEED_CD_SPEED) {
        r1 = esp_err_to_name(atapi_host_set_speed(P.dev, kbps));
    }
    if (P.speed_methods & PLAYER_SPEED_STREAMING) {
        r2 = esp_err_to_name(atapi_host_set_streaming(P.dev, end, P.speed_kbps ? kbps : 0xFFFF));
    }
    uint16_t cur = 0, max = 0;
    esp_err_t e3 = atapi_host_get_speed(P.dev, &cur, &max);
    ESP_LOGI(TAG, "Read speed %u kB/s: SET CD SPEED %s, SET STREAMING %s; drive reports%s cur %u max %u kB/s",
             kbps, r1, r2, e3 == ESP_OK ? "" : " (no page 2Ah)", cur, max);
}

void player_set_read_speed(uint16_t kbps, uint8_t methods)
{
    xSemaphoreTake(P.dev_lock, portMAX_DELAY);
    P.speed_kbps = kbps;
    P.speed_methods = methods;
    if (P.dev && P.has_disc) {
        apply_read_speed();
    }
    xSemaphoreGive(P.dev_lock);
}

static void manage_spindown(bool want_read)
{
    LOCK();
    bool playing = P.state == PLAYER_PLAYING;
    bool can = P.has_disc && P.dev;
    int64_t now = esp_timer_get_time();
    if (playing) {
        P.idle_since = 0;
    } else if (!P.idle_since) {
        P.idle_since = now;
    }
    bool spin_down = can && !playing && !P.spun_down && now - P.idle_since > (int64_t)SPINDOWN_S * 1000000;
    bool spin_up = can && P.spun_down && (playing || want_read);
    UNLOCK();
    if (!spin_down && !spin_up) {
        return;
    }
    xSemaphoreTake(P.dev_lock, portMAX_DELAY);
    if (P.dev) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = atapi_host_start_stop(P.dev, spin_up, false);
        ESP_LOGI(TAG, "Drive %s: %s (%lld ms)", spin_up ? "spin-up" : "spin-down (idle)", esp_err_to_name(err),
                 (esp_timer_get_time() - t0) / 1000);
        P.spun_down = spin_down; /* even on error: a read spins the drive up anyway, or fails visibly */
        if (spin_up) {
            apply_read_speed();
        }
    }
    xSemaphoreGive(P.dev_lock);
}

bool player_is_spun_down(void)
{
    return P.spun_down;
}

static void reader_task(void *arg)
{
    while (true) {
        LOCK();
        bool active = P.has_disc && P.dev && (P.state == PLAYER_PLAYING || P.state == PLAYER_PAUSED) &&
                      !P.reader_done;
        uint32_t lba = P.read_lba;
        uint32_t gen = P.gen;
        uint32_t end = active ? audio_end_from(lba) : 0;
        if (active && lba >= end) {
            /* Reached a data track or the lead-out: continue at the next audio track, if any */
            uint32_t next = skip_data_tracks(end);
            if (next < P.toc.leadout_lba && next != lba) {
                P.read_lba = next;
                UNLOCK();
                continue;
            }
            P.reader_done = true;
            active = false;
        }
        UNLOCK();

        manage_spindown(false);
        if (!active) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }

        chunk_t *c;
        if (xQueueReceive(P.free_q, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue; /* buffer full; re-check state */
        }

        manage_spindown(true); /* paused with free buffer space, or playing: the drive must turn */
        uint32_t n = MIN((uint32_t)CHUNK_SECTORS, end - lba);
        esp_err_t err = ESP_FAIL;
        int64_t t0 = esp_timer_get_time();
        xSemaphoreTake(P.dev_lock, portMAX_DELAY);
        if (P.dev) {
            for (int attempt = 0; attempt < READ_RETRIES && err != ESP_OK; attempt++) {
                err = atapi_host_read_cd_audio(P.dev, lba, n, c->data);
                atapi_sense_t sense;
                if (err == ESP_ERR_ATAPI_CHECK_CONDITION && atapi_host_get_last_sense(P.dev, &sense) == ESP_OK &&
                        sense.key == ATAPI_SENSE_NOT_READY && sense.asc == 0x04) {
                    /* Motor stopped (by us, or the drive's own standby timer): spin up, then retry */
                    ESP_LOGI(TAG, "Drive not spinning (%s), starting it", atapi_sense_to_str(&sense));
                    atapi_host_start_stop(P.dev, true, false);
                    P.spun_down = false;
                    apply_read_speed();
                    err = atapi_host_read_cd_audio(P.dev, lba, n, c->data);
                }
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Read LBA %"PRIu32"+%"PRIu32" attempt %d failed: %s (0x%x)", lba, n, attempt + 1,
                             esp_err_to_name(err), err);
                }
            }
        }
        xSemaphoreGive(P.dev_lock);
        int64_t read_ms = (esp_timer_get_time() - t0) / 1000;
        if (read_ms > SLOW_READ_MS) {
            /* Normal reads take ~60 ms; long ones mean the drive is retrying, seeking or spinning up */
            ESP_LOGW(TAG, "Slow read at LBA %"PRIu32": %lld ms (buffer %"PRIu32" ms)", lba, read_ms,
                     P.buffered * 1000 / ATAPI_CD_FRAMES_PER_SECOND);
        }

        LOCK();
        if (err != ESP_OK) {
            P.read_errors++;
            ESP_LOGW(TAG, "Read error at LBA %"PRIu32" (%s), inserting silence", lba, esp_err_to_name(err));
            memset(c->data, 0, n * ATAPI_CD_AUDIO_SECTOR_SIZE);
        }
        if (gen == P.gen) {
            c->lba = lba;
            c->count = n;
            c->gen = gen;
            P.read_lba = lba + n;
            P.buffered += n;
            UNLOCK();
            xQueueSend(P.filled_q, &c, portMAX_DELAY);
        } else {
            UNLOCK();
            xQueueSend(P.free_q, &c, portMAX_DELAY); /* seek happened meanwhile */
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Writer                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * De-emphasis for CDs mastered with pre-emphasis (TOC control bit; 50/15 us, -10 dB shelf above ~10 kHz).
 * First-order IIR H(z) = (b0 + b1 z^-1) / (1 + a1 z^-1) at 44.1 kHz. The coefficients are a minimax fit to
 * the analog curve (1 + s*15us) / (1 + s*50us), 0 dB at DC: max. 0.07 dB error over 20 Hz..20 kHz
 * (plain bilinear transform: 0.9 dB).
 */
#define DEEMPH_B0  0.460405558f
#define DEEMPH_B1 -0.089248819f
#define DEEMPH_A1 -0.628843262f

typedef struct {
    float x1[2], y1[2];
} deemph_state_t;

static void apply_deemphasis(int16_t *s, size_t frames, deemph_state_t *st)
{
    for (size_t i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = s[2 * i + ch];
            float y = DEEMPH_B0 * x + DEEMPH_B1 * st->x1[ch] - DEEMPH_A1 * st->y1[ch];
            st->x1[ch] = x;
            st->y1[ch] = y;
            s[2 * i + ch] = (int16_t)(y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : lrintf(y));
        }
    }
}

static void apply_volume(int16_t *s, size_t n, int volume)
{
    if (volume >= 100) {
        return;
    }
    /* Roughly perceptual curve: gain = (v/100)^2 */
    int32_t gain = (int32_t)volume * volume * 32768 / 10000;
    for (size_t i = 0; i < n; i++) {
        s[i] = (int16_t)(((int32_t)s[i] * gain) >> 15);
    }
}

static void writer_task(void *arg)
{
    chunk_t *c = NULL;
    uint32_t offset = 0; /* sectors of c already played */
    deemph_state_t deemph = {0};
    uint32_t next_lba = UINT32_MAX; /* LBA expected next: a jump resets the filter state */

    while (true) {
        LOCK();
        player_state_t state = P.state;
        uint32_t gen = P.gen;
        int volume = P.volume;
        UNLOCK();

        if (c && c->gen != gen) {
            xQueueSend(P.free_q, &c, portMAX_DELAY); /* stale */
            c = NULL;
        }
        if (state != PLAYER_PLAYING) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)); /* I2S outputs silence (auto clear) */
            continue;
        }
        if (!c) {
            if (xQueueReceive(P.filled_q, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
                c = NULL;
                LOCK();
                if (P.state == PLAYER_PLAYING && P.gen == gen) {
                    if (P.reader_done) {
                        ESP_LOGI(TAG, "End of disc");
                        P.state = PLAYER_STOPPED;
                        int first = first_audio_track(0);
                        seek_locked(first >= 0 ? P.toc.tracks[first].lba : 0);
                    } else if (P.primed) {
                        P.underruns++;
                    }
                }
                UNLOCK();
                continue;
            }
            offset = 0;
            if (c->gen != gen) {
                continue; /* stale, released at top of loop */
            }
        }

        uint32_t n = MIN((uint32_t)SLICE_SECTORS, c->count - offset);
        int16_t *pcm = (int16_t *)(c->data + offset * ATAPI_CD_AUDIO_SECTOR_SIZE);
        size_t bytes = n * ATAPI_CD_AUDIO_SECTOR_SIZE;
        LOCK();
        bool emph = P.force_deemph || track_has_preemphasis(c->lba + offset);
        UNLOCK();
        if (emph) {
            if (c->lba + offset != next_lba) {
                memset(&deemph, 0, sizeof(deemph));
            }
            apply_deemphasis(pcm, bytes / 4, &deemph);
        }
        next_lba = c->lba + offset + n;
        apply_volume(pcm, bytes / 2, volume);

        LOCK();
        if (c->gen == P.gen) {
            P.play_lba = c->lba + offset;
            P.primed = true;
            P.buffered -= MIN(P.buffered, n);
        }
        UNLOCK();

        audio_out_write(pcm, bytes, pdMS_TO_TICKS(1000));
        offset += n;
        if (offset >= c->count) {
            xQueueSend(P.free_q, &c, portMAX_DELAY);
            c = NULL;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Volume persistence (NVS)                                                 */
/* ------------------------------------------------------------------------ */

static int volume_load(void)
{
    nvs_handle_t nvs;
    uint8_t v = PLAYER_VOLUME_MAX;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_u8(nvs, NVS_KEY_VOLUME, &v) != ESP_OK) {
            v = PLAYER_VOLUME_MAX;
        }
        nvs_close(nvs);
    }
    int volume = MIN((int)v, PLAYER_VOLUME_MAX); /* the cap may have been lowered since it was saved */
    ESP_LOGI(TAG, "Volume %d%% (stored %d, max %d)", player_volume_display(volume), v, PLAYER_VOLUME_MAX);
    return volume;
}

/* Runs in the esp_timer task, VOLUME_SAVE_DELAY_MS after the last volume change */
static void volume_save_cb(void *arg)
{
    LOCK();
    uint8_t v = (uint8_t)P.volume;
    UNLOCK();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        uint8_t old;
        if (nvs_get_u8(nvs, NVS_KEY_VOLUME, &old) != ESP_OK || old != v) {
            err = nvs_set_u8(nvs, NVS_KEY_VOLUME, v);
            if (err == ESP_OK) {
                err = nvs_commit(nvs);
            }
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving volume failed: %s", esp_err_to_name(err));
    }
}

/** Change the volume and schedule saving it (call with lock held) */
static void volume_change_locked(int delta)
{
    int v = MIN(PLAYER_VOLUME_MAX, MAX(0, P.volume + delta));
    if (v != P.volume) {
        P.volume = v;
        esp_timer_stop(P.volume_save_timer); /* restart the debounce period */
        esp_timer_start_once(P.volume_save_timer, VOLUME_SAVE_DELAY_MS * 1000);
    }
}

static bool autoplay_load(void)
{
    nvs_handle_t nvs;
    uint8_t v = 1;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_AUTOPLAY, &v); /* keeps the default if not stored */
        nvs_close(nvs);
    }
    return v != 0;
}

bool player_get_autoplay(void)
{
    LOCK();
    bool on = P.autoplay;
    UNLOCK();
    return on;
}

void player_set_autoplay(bool on)
{
    LOCK();
    P.autoplay = on;
    UNLOCK();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, NVS_KEY_AUTOPLAY, on);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving autoplay failed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t player_init(void)
{
    P.lock = xSemaphoreCreateMutex();
    P.dev_lock = xSemaphoreCreateMutex();
    P.speed_kbps = READ_SPEED_KBPS;
    P.speed_methods = PLAYER_SPEED_CD_SPEED;
    P.free_q = xQueueCreate(NUM_CHUNKS, sizeof(chunk_t *));
    P.filled_q = xQueueCreate(NUM_CHUNKS, sizeof(chunk_t *));
    ESP_RETURN_ON_FALSE(P.lock && P.dev_lock && P.free_q && P.filled_q, ESP_ERR_NO_MEM, TAG, "");
    for (int i = 0; i < NUM_CHUNKS; i++) {
        P.chunks[i].data = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(P.chunks[i].data, ESP_ERR_NO_MEM, TAG, "chunk alloc");
        chunk_t *c = &P.chunks[i];
        xQueueSend(P.free_q, &c, 0);
    }
    P.state = PLAYER_NO_DISC;
    P.volume = volume_load();
    P.autoplay = autoplay_load();
    strlcpy(P.message, "Waiting for drive", sizeof(P.message));

    const esp_timer_create_args_t save_timer_args = {
        .callback = volume_save_cb,
        .name = "vol_save",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&save_timer_args, &P.volume_save_timer), TAG, "timer");

    ESP_RETURN_ON_ERROR(audio_out_init(), TAG, "audio");
    xTaskCreate(reader_task, "cd_reader", 4096, NULL, 6, &P.reader_task);
    xTaskCreate(writer_task, "cd_writer", 4096, NULL, 7, &P.writer_task);
    ESP_LOGI(TAG, "Player ready (%d x %d KB read-ahead in PSRAM)", NUM_CHUNKS, CHUNK_BYTES / 1024);
    return ESP_OK;
}

void player_set_device(atapi_host_device_handle_t dev)
{
    xSemaphoreTake(P.dev_lock, portMAX_DELAY); /* wait for a running read */
    LOCK();
    P.dev = dev;
    UNLOCK();
    xSemaphoreGive(P.dev_lock);
}

void player_load_disc(const atapi_toc_t *toc, atapi_cd_text_t *cdtext)
{
    LOCK();
    if (P.cdtext) {
        atapi_cd_text_free(P.cdtext);
        free(P.cdtext);
    }
    free(P.meta);
    P.meta = NULL;
    P.toc = *toc;
    P.cdtext = cdtext;
    P.spun_down = false;
    P.idle_since = 0;
    P.has_disc = true;
    P.state = PLAYER_STOPPED;
    P.underruns = 0;
    P.read_errors = 0;
    int first = first_audio_track(0);
    seek_locked(first >= 0 ? toc->tracks[first].lba : 0);
    UNLOCK();
    xSemaphoreTake(P.dev_lock, portMAX_DELAY);
    if (P.dev) {
        apply_read_speed();
    }
    xSemaphoreGive(P.dev_lock);
}

void player_unload_disc(void)
{
    LOCK();
    P.has_disc = false;
    P.state = PLAYER_NO_DISC;
    P.gen++;
    drain_locked();
    if (P.cdtext) {
        atapi_cd_text_free(P.cdtext);
        free(P.cdtext);
        P.cdtext = NULL;
    }
    free(P.meta);
    P.meta = NULL;
    memset(&P.toc, 0, sizeof(P.toc));
    UNLOCK();
}

void player_set_metadata(player_meta_t *meta)
{
    LOCK();
    free(P.meta);
    P.meta = P.has_disc ? meta : NULL;
    UNLOCK();
    if (!P.has_disc) {
        free(meta);
    }
}

void player_force_deemphasis(bool on)
{
    LOCK();
    P.force_deemph = on;
    UNLOCK();
    ESP_LOGI(TAG, "De-emphasis forced on all tracks: %s", on ? "on" : "off");
}

bool player_deemphasis_forced(void)
{
    return P.force_deemph;
}

void player_set_message(const char *msg)
{
    LOCK();
    strlcpy(P.message, msg ? msg : "", sizeof(P.message));
    UNLOCK();
}

void player_command(player_cmd_t cmd, int arg)
{
    LOCK();
    if (cmd == PLAYER_CMD_VOLUME_REL || cmd == PLAYER_CMD_VOLUME_SET) { /* allowed without a disc */
        volume_change_locked(cmd == PLAYER_CMD_VOLUME_SET ? arg - P.volume : arg);
        UNLOCK();
        return;
    }
    if (!P.has_disc) {
        UNLOCK();
        return;
    }
    int idx = track_index_for_lba(P.play_lba);
    uint32_t track_start = P.toc.tracks[idx].lba;

    switch (cmd) {
    case PLAYER_CMD_PLAY_PAUSE:
        P.state = (P.state == PLAYER_PLAYING) ? PLAYER_PAUSED : PLAYER_PLAYING;
        break;
    case PLAYER_CMD_PLAY:
        P.state = PLAYER_PLAYING;
        break;
    case PLAYER_CMD_PAUSE:
        if (P.state == PLAYER_PLAYING) {
            P.state = PLAYER_PAUSED;
        }
        break;
    case PLAYER_CMD_STOP: {
        P.state = PLAYER_STOPPED;
        int first = first_audio_track(0);
        seek_locked(first >= 0 ? P.toc.tracks[first].lba : 0);
        break;
    }
    case PLAYER_CMD_NEXT: {
        int next = first_audio_track(idx + 1);
        if (next >= 0) {
            seek_locked(P.toc.tracks[next].lba);
        }
        break;
    }
    case PLAYER_CMD_PREV:
        if (P.play_lba - track_start > PREV_RESTART_SECTORS || idx == 0) {
            seek_locked(track_start);
        } else {
            int prev = idx - 1;
            while (prev >= 0 && !track_is_audio(prev)) {
                prev--;
            }
            seek_locked(prev >= 0 ? P.toc.tracks[prev].lba : track_start);
        }
        break;
    case PLAYER_CMD_TRACK:
        for (int i = 0; i < P.toc.num_tracks; i++) {
            if (P.toc.tracks[i].number == arg && track_is_audio(i)) {
                seek_locked(P.toc.tracks[i].lba);
                if (P.state != PLAYER_PAUSED) {
                    P.state = PLAYER_PLAYING;
                }
                break;
            }
        }
        break;
    case PLAYER_CMD_SEEK_REL: {
        int64_t target = (int64_t)P.play_lba + (int64_t)arg * ATAPI_CD_FRAMES_PER_SECOND;
        target = MAX(target, (int64_t)P.toc.tracks[0].lba);
        target = MIN(target, (int64_t)P.toc.leadout_lba - 1);
        seek_locked((uint32_t)target);
        break;
    }
    case PLAYER_CMD_SEEK_ABS: {
        uint32_t len = atapi_toc_track_length(&P.toc, idx);
        uint32_t off = (uint32_t)MAX(arg, 0) * ATAPI_CD_FRAMES_PER_SECOND;
        seek_locked(track_start + MIN(off, len ? len - 1 : 0));
        break;
    }
    case PLAYER_CMD_VOLUME_REL:
    case PLAYER_CMD_VOLUME_SET:
        break; /* handled above */
    }
    UNLOCK();
    xTaskNotifyGive(P.reader_task);
    xTaskNotifyGive(P.writer_task);
}

/** Copy UTF-8 text, truncated to PLAYER_TEXT_LEN at a character boundary (never mid-sequence) */
static void copy_text(char *dst, const char *src)
{
    src = src ? src : "";
    size_t n = strlen(src);
    if (n >= PLAYER_TEXT_LEN) {
        n = PLAYER_TEXT_LEN - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--; /* src[n] continues a sequence: cut before the sequence's first byte */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/** CD-TEXT field if present, else the metadata field (may be NULL) */
static const char *cdtext_or_meta(atapi_cd_text_field_t field, int track, const char *meta_val)
{
    const char *v = P.cdtext ? P.cdtext->text[field][track] : NULL;
    return (v && v[0]) ? v : (meta_val && meta_val[0]) ? meta_val : NULL;
}

static const char *album_text(void)
{
    return cdtext_or_meta(ATAPI_CDTEXT_TITLE, 0, P.meta ? P.meta->album : NULL);
}

static const char *artist_text(void)
{
    return cdtext_or_meta(ATAPI_CDTEXT_PERFORMER, 0, P.meta ? P.meta->artist : NULL);
}

static const char *track_title(int n)
{
    return cdtext_or_meta(ATAPI_CDTEXT_TITLE, n, P.meta ? P.meta->track[n].title : NULL);
}

/** Track performer, NULL if the same as the album artist */
static const char *track_performer(int n)
{
    const char *perf = cdtext_or_meta(ATAPI_CDTEXT_PERFORMER, n, P.meta ? P.meta->track[n].artist : NULL);
    const char *artist = artist_text();
    return (perf && (!artist || strcmp(perf, artist) != 0)) ? perf : NULL;
}

void player_get_status(player_status_t *st)
{
    memset(st, 0, sizeof(*st));
    LOCK();
    st->state = P.state;
    st->volume = P.volume;
    st->underruns = P.underruns;
    st->read_errors = P.read_errors;
    st->buffer_pct = MIN(100, P.buffered * 100 / (NUM_CHUNKS * CHUNK_SECTORS));
    st->buffer_ms = P.buffered * 1000 / ATAPI_CD_FRAMES_PER_SECOND;
    copy_text(st->message, P.message);
    if (P.has_disc && P.toc.num_tracks > 0) {
        int idx = track_index_for_lba(P.play_lba);
        const atapi_toc_entry_t *t = &P.toc.tracks[idx];
        st->num_tracks = P.toc.num_tracks;
        st->track = t->number;
        st->track_pos = P.play_lba - t->lba;
        st->track_len = atapi_toc_track_length(&P.toc, idx);
        st->disc_len = P.toc.leadout_lba;
        st->preemphasis = atapi_toc_entry_has_preemphasis(t);
        st->deemphasis = st->preemphasis || P.force_deemph;

        copy_text(st->album, album_text());
        copy_text(st->artist, artist_text());
        copy_text(st->title, track_title(t->number));
        copy_text(st->performer, track_performer(t->number));
        if (!st->title[0]) {
            snprintf(st->title, PLAYER_TEXT_LEN, "%s %d", atapi_toc_entry_is_data(t) ? "Data track" : "Track",
                     t->number);
        }
    }
    UNLOCK();
}

int player_get_tracks(player_track_info_t *out, int max)
{
    LOCK();
    int n = P.has_disc ? MIN(P.toc.num_tracks, max) : 0;
    for (int i = 0; i < n; i++) {
        const atapi_toc_entry_t *t = &P.toc.tracks[i];
        player_track_info_t *o = &out[i];
        o->number = t->number;
        o->audio = !atapi_toc_entry_is_data(t);
        o->length = atapi_toc_track_length(&P.toc, i);
        copy_text(o->title, track_title(t->number));
        copy_text(o->performer, track_performer(t->number));
    }
    UNLOCK();
    return n;
}
