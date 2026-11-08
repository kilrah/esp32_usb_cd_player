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
    char message[PLAYER_TEXT_LEN];

    uint32_t gen;
    uint32_t read_lba;      /* next sector to read */
    uint32_t play_lba;      /* sector currently being played */
    bool reader_done;       /* reached the end of the playable area */
    bool primed;            /* audio has been played since the last seek (for underrun counting) */
    uint32_t buffered;      /* sectors read ahead and not yet sent to I2S (current generation) */
    int volume;
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
    P.volume = PLAYER_VOLUME_MAX;
    strlcpy(P.message, "Waiting for drive", sizeof(P.message));

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
    memset(&P.toc, 0, sizeof(P.toc));
    UNLOCK();
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
    if (cmd == PLAYER_CMD_VOLUME_REL) { /* allowed without a disc */
        P.volume = MIN(PLAYER_VOLUME_MAX, MAX(0, P.volume + arg));
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
    case PLAYER_CMD_VOLUME_REL:
        P.volume = MIN(PLAYER_VOLUME_MAX, MAX(0, P.volume + arg));
        break;
    }
    UNLOCK();
    xTaskNotifyGive(P.reader_task);
    xTaskNotifyGive(P.writer_task);
}

static void copy_text(char *dst, const char *src)
{
    strlcpy(dst, src ? src : "", PLAYER_TEXT_LEN);
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

        const atapi_cd_text_t *ct = P.cdtext;
        if (ct) {
            copy_text(st->album, ct->text[ATAPI_CDTEXT_TITLE][0]);
            copy_text(st->artist, ct->text[ATAPI_CDTEXT_PERFORMER][0]);
            copy_text(st->title, ct->text[ATAPI_CDTEXT_TITLE][t->number]);
            const char *perf = ct->text[ATAPI_CDTEXT_PERFORMER][t->number];
            if (perf && strcmp(perf, st->artist) != 0) {
                copy_text(st->performer, perf);
            }
        }
        if (!st->title[0]) {
            snprintf(st->title, PLAYER_TEXT_LEN, "%s %d", atapi_toc_entry_is_data(t) ? "Data track" : "Track",
                     t->number);
        }
    }
    UNLOCK();
}
