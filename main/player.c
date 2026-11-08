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
#define NUM_CHUNKS      12                                  /* ~2.5 s read-ahead, 450 KB PSRAM */
#define SLICE_SECTORS   2                                   /* I2S write granularity (~27 ms) */
#define READ_RETRIES    3
#define PREV_RESTART_SECTORS (3 * ATAPI_CD_FRAMES_PER_SECOND)
#define SLOW_READ_MS    500                                 /* log reads slower than this */
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
    bool primed;            /* audio has been played since the last seek (for underrun counting) */
    uint32_t buffered;      /* sectors read ahead and not yet sent to I2S (current generation) */
    esp_timer_handle_t volume_save_timer;
    int volume;
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

        if (!active) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }

        chunk_t *c;
        if (xQueueReceive(P.free_q, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue; /* buffer full; re-check state */
        }

        uint32_t n = MIN((uint32_t)CHUNK_SECTORS, end - lba);
        esp_err_t err = ESP_FAIL;
        int64_t t0 = esp_timer_get_time();
        xSemaphoreTake(P.dev_lock, portMAX_DELAY);
        if (P.dev) {
            for (int attempt = 0; attempt < READ_RETRIES && err != ESP_OK; attempt++) {
                err = atapi_host_read_cd_audio(P.dev, lba, n, c->data);
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
    P.has_disc = true;
    P.state = PLAYER_STOPPED;
    P.underruns = 0;
    P.read_errors = 0;
    int first = first_audio_track(0);
    seek_locked(first >= 0 ? toc->tracks[first].lba : 0);
    UNLOCK();
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

static void copy_text(char *dst, const char *src)
{
    strlcpy(dst, src ? src : "", PLAYER_TEXT_LEN);
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
