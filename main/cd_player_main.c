/*
 * USB CD reader - test application for the usb_host_atapi component
 *
 * - Detects an optical drive connected to the USB Host port
 * - Waits for an audio CD, reads and prints the TOC
 * - Reads a few seconds of CD-DA audio and reports throughput and levels
 * - BOOT button ejects / loads the tray
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <sys/param.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "usb/atapi_host.h"
#include "status_led.h"

static const char *TAG = "cd";

#define BUTTON_PIN          GPIO_NUM_0  /* BOOT button: eject / load */
#define STATUS_LED_PIN      48          /* On-board WS2812B */
#define READ_CHUNK_SECTORS  16          /* sectors per read call (37632 bytes) */
#define TEST_READ_SECONDS   5

typedef enum {
    APP_DEVICE_CONNECTED,
    APP_DEVICE_DISCONNECTED,
    APP_BUTTON,
} app_msg_id_t;

typedef struct {
    app_msg_id_t id;
    union {
        uint8_t address;
        atapi_host_device_handle_t handle;
    };
} app_msg_t;

static QueueHandle_t s_queue;

/* ------------------------------------------------------------------------ */

static void atapi_event_cb(const atapi_host_event_t *event, void *arg)
{
    app_msg_t msg;
    if (event->event == ATAPI_DEVICE_CONNECTED) {
        msg.id = APP_DEVICE_CONNECTED;
        msg.address = event->device.address;
    } else {
        msg.id = APP_DEVICE_DISCONNECTED;
        msg.handle = event->device.handle;
    }
    xQueueSend(s_queue, &msg, portMAX_DELAY);
}

static void IRAM_ATTR button_isr(void *arg)
{
    static int64_t last_us;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 300000) { /* debounce */
        return;
    }
    last_us = now;
    BaseType_t woken = pdFALSE;
    app_msg_t msg = {.id = APP_BUTTON};
    xQueueSendFromISR(s_queue, &msg, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LOWMED};
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const atapi_host_driver_config_t atapi_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = atapi_event_cb,
    };
    ESP_ERROR_CHECK(atapi_host_install(&atapi_config));

    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* ------------------------------------------------------------------------ */

static void lba_to_msf(uint32_t lba, int *m, int *s, int *f)
{
    *f = lba % ATAPI_CD_FRAMES_PER_SECOND;
    *s = (lba / ATAPI_CD_FRAMES_PER_SECOND) % 60;
    *m = lba / ATAPI_CD_FRAMES_PER_SECOND / 60;
}

static const char *cdtext_get(const atapi_cd_text_t *cdt, atapi_cd_text_field_t field, int track)
{
    return (cdt && track >= 0 && track <= ATAPI_MAX_TRACKS) ? cdt->text[field][track] : NULL;
}

static void print_toc(const atapi_toc_t *toc, const atapi_cd_text_t *cdt)
{
    int m, s, f;
    lba_to_msf(toc->leadout_lba, &m, &s, &f);
    printf("\n");
    const char *album = cdtext_get(cdt, ATAPI_CDTEXT_TITLE, 0);
    const char *artist = cdtext_get(cdt, ATAPI_CDTEXT_PERFORMER, 0);
    if (album || artist) {
        printf("Album:  %s\nArtist: %s\n", album ? album : "-", artist ? artist : "-");
    }
    const char *upc = cdtext_get(cdt, ATAPI_CDTEXT_UPC_ISRC, 0);
    if (upc) {
        printf("UPC/EAN: %s\n", upc);
    }
    printf("TOC: tracks %d-%d, total %02d:%02d.%02d (%"PRIu32" sectors)%s\n",
           toc->first_track, toc->last_track, m, s, f, toc->leadout_lba, cdt ? "" : ", no CD-TEXT");
    printf(" #   type   start LBA   length\n");
    for (int i = 0; i < toc->num_tracks; i++) {
        const atapi_toc_entry_t *t = &toc->tracks[i];
        lba_to_msf(atapi_toc_track_length(toc, i), &m, &s, &f);
        printf("%2d   %-5s %10"PRIu32"   %02d:%02d.%02d", t->number,
               atapi_toc_entry_is_data(t) ? "data" : "audio", t->lba, m, s, f);
        const char *title = cdtext_get(cdt, ATAPI_CDTEXT_TITLE, t->number);
        const char *perf = cdtext_get(cdt, ATAPI_CDTEXT_PERFORMER, t->number);
        if (title) {
            printf("   %s", title);
        }
        if (perf && (!artist || strcmp(perf, artist) != 0)) {
            printf(" - %s", perf); /* only when it differs from the album artist */
        }
        if (atapi_toc_entry_has_preemphasis(t)) {
            printf("  (pre-emphasis)");
        }
        printf("\n");
    }
    printf("\n");
}

/** Read a few seconds of the first audio track, report throughput and signal levels */
static void audio_read_test(atapi_host_device_handle_t dev, const atapi_toc_t *toc)
{
    int idx = -1;
    for (int i = 0; i < toc->num_tracks; i++) {
        if (!atapi_toc_entry_is_data(&toc->tracks[i])) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        ESP_LOGW(TAG, "No audio tracks on this disc");
        return;
    }

    const atapi_toc_entry_t *track = &toc->tracks[idx];
    uint32_t total = MIN(atapi_toc_track_length(toc, idx), (uint32_t)(TEST_READ_SECONDS * ATAPI_CD_FRAMES_PER_SECOND));
    const size_t buf_size = READ_CHUNK_SECTORS * ATAPI_CD_AUDIO_SECTOR_SIZE;
    int16_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for audio buffer");
        return;
    }

    ESP_LOGI(TAG, "Reading %"PRIu32" sectors (%.1f s) of track %d from LBA %"PRIu32"...",
             total, total / (float)ATAPI_CD_FRAMES_PER_SECOND, track->number, track->lba);

    /* Spin-up / seek latency of the first read is measured separately */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = atapi_host_read_cd_audio(dev, track->lba, 1, buf);
    int64_t t_first = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "First READ CD failed: %s (0x%x)", esp_err_to_name(err), err);
        free(buf);
        return;
    }

    int16_t peak_l = 0, peak_r = 0;
    double sum_sq = 0;
    uint64_t samples = 0;
    uint32_t lba = track->lba, done = 0;

    t0 = esp_timer_get_time();
    while (done < total) {
        uint32_t n = MIN(total - done, (uint32_t)READ_CHUNK_SECTORS);
        err = atapi_host_read_cd_audio(dev, lba, n, buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "READ CD at LBA %"PRIu32" failed: %s (0x%x)", lba, esp_err_to_name(err), err);
            break;
        }
        size_t frames = n * ATAPI_CD_AUDIO_SECTOR_SIZE / 4;
        for (size_t i = 0; i < frames; i++) {
            int16_t l = buf[2 * i], r = buf[2 * i + 1];
            peak_l = MAX(peak_l, (int16_t)abs(l));
            peak_r = MAX(peak_r, (int16_t)abs(r));
            sum_sq += (double)l * l + (double)r * r;
        }
        samples += frames * 2;
        lba += n;
        done += n;
    }
    int64_t elapsed = esp_timer_get_time() - t0;

    if (done > 0) {
        float kbps = done * ATAPI_CD_AUDIO_SECTOR_SIZE / 1024.0f / (elapsed / 1e6f);
        float rms = sqrt(sum_sq / samples);
        ESP_LOGI(TAG, "First sector latency: %lld ms", t_first / 1000);
        ESP_LOGI(TAG, "Read %"PRIu32" sectors in %lld ms: %.1f KiB/s = %.2fx realtime",
                 done, elapsed / 1000, kbps, kbps / (44100 * 4 / 1024.0f));
        ESP_LOGI(TAG, "Peak L %d (%.1f dBFS), peak R %d (%.1f dBFS), RMS %.1f dBFS",
                 peak_l, 20 * log10f((peak_l + 1) / 32768.0f),
                 peak_r, 20 * log10f((peak_r + 1) / 32768.0f),
                 20 * log10f((rms + 1) / 32768.0f));
        ESP_LOG_BUFFER_HEX(TAG, buf, 32);
    }
    free(buf);
}

static bool on_disc_ready(atapi_host_device_handle_t dev)
{
    static atapi_toc_t toc; /* ~1 KB, keep off the stack */
    status_led_set(STATUS_BUSY);
    esp_err_t err = atapi_host_read_toc(dev, &toc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "READ TOC failed: %s (0x%x)", esp_err_to_name(err), err);
        return false;
    }
    static atapi_cd_text_t cdtext; /* ~3.3 KB */
    err = atapi_host_read_cd_text(dev, &cdtext);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "READ CD-TEXT failed: %s (0x%x)", esp_err_to_name(err), err);
    }
    print_toc(&toc, err == ESP_OK ? &cdtext : NULL);
    atapi_cd_text_free(&cdtext);    audio_read_test(dev, &toc);
    return true;
}

/* ------------------------------------------------------------------------ */

void app_main(void)
{
    s_queue = xQueueCreate(8, sizeof(app_msg_t));
    assert(s_queue);

    if (status_led_init(STATUS_LED_PIN) != ESP_OK) {
        ESP_LOGW(TAG, "Status LED not available");
    }


    const gpio_config_t btn = {
        .pin_bit_mask = BIT64(BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL));

    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Waiting for USB optical drive...");
    status_led_set(STATUS_WAIT_DRIVE);

    atapi_host_device_handle_t dev = NULL;
    bool disc_ready = false;
    bool tray_open = false;
    esp_err_t last_state = ESP_FAIL;

    while (true) {
        app_msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (msg.id) {
            case APP_DEVICE_CONNECTED: {
                if (dev) {
                    ESP_LOGW(TAG, "Only one drive supported, ignoring device %d", msg.address);
                    break;
                }
                esp_err_t err = atapi_host_install_device(msg.address, &dev);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Device %d is not a usable optical drive: %s", msg.address, esp_err_to_name(err));
                    dev = NULL;
                    status_led_set(STATUS_ERROR);
                    break;
                }
                status_led_set(STATUS_DISC_LOADING);
                atapi_host_device_info_t info;
                atapi_host_get_device_info(dev, &info);
                ESP_LOGI(TAG, "Optical drive connected: %s %s (rev %s), VID 0x%04x PID 0x%04x",
                         info.vendor, info.product, info.revision, info.vid, info.pid);
                disc_ready = false;
                last_state = ESP_FAIL;
                break;
            }
            case APP_DEVICE_DISCONNECTED:
                if (dev && msg.handle == dev) {
                    ESP_LOGI(TAG, "Optical drive disconnected");
                    atapi_host_uninstall_device(dev);
                    dev = NULL;
                    disc_ready = false;
                }
                status_led_set(STATUS_WAIT_DRIVE);
                break;
            case APP_BUTTON:
                /* Ignore glitches: the button must still be held after a short delay */
                vTaskDelay(pdMS_TO_TICKS(50));
                if (gpio_get_level(BUTTON_PIN) != 0) {
                    break;
                }
                if (dev) {
                    tray_open = !tray_open;
                    ESP_LOGI(TAG, "%s tray", tray_open ? "Ejecting" : "Loading");
                    status_led_set(STATUS_DISC_LOADING);
                    esp_err_t err = tray_open ? atapi_host_eject(dev) : atapi_host_load(dev);
                    if (err != ESP_OK) {
                        atapi_sense_t sense;
                        atapi_host_get_last_sense(dev, &sense);
                        ESP_LOGW(TAG, "Tray command failed: %s (0x%x), sense %X/%02X/%02X (%s)",
                                 esp_err_to_name(err), err, sense.key, sense.asc, sense.ascq,
                                 atapi_sense_to_str(&sense));
                    }
                    disc_ready = false;
                }
                break;
            }
            continue;
        }

        /* Periodic disc presence polling */
        if (!dev) {
            continue;
        }
        esp_err_t state = atapi_host_wait_ready(dev, 1000);
        if (state != last_state) {
            if (state == ESP_ERR_ATAPI_NO_MEDIUM) {
                atapi_sense_t s;
                atapi_host_get_last_sense(dev, &s);
                ESP_LOGI(TAG, "No disc (%s)", atapi_sense_to_str(&s));
                if (s.ascq == 0x02) {
                    tray_open = true;
                } else if (s.ascq == 0x01) {
                    tray_open = false;
                }
            } else if (state == ESP_ERR_ATAPI_NOT_READY) {
                ESP_LOGI(TAG, "Disc becoming ready...");
            } else if (state != ESP_OK) {
                ESP_LOGW(TAG, "Drive state: %s (0x%x)", esp_err_to_name(state), state);
            }
            last_state = state;
        }
        if (state == ESP_ERR_ATAPI_NO_MEDIUM) {
            status_led_set(STATUS_NO_DISC);
        } else if (state == ESP_ERR_ATAPI_NOT_READY) {
            status_led_set(STATUS_DISC_LOADING);
        } else if (state != ESP_OK) {
            status_led_set(STATUS_ERROR);
        }
        if (state == ESP_OK && !disc_ready) {
            disc_ready = true;
            tray_open = false;
            ESP_LOGI(TAG, "Disc ready");
            status_led_set(on_disc_ready(dev) ? STATUS_DISC_READY : STATUS_ERROR);
        } else if (state != ESP_OK) {
            disc_ready = false;
        }
    }
}
