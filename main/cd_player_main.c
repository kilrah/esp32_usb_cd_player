/*
 * USB CD player (LilyGO T-Display S3 + USB optical drive + PCM5102 I2S DAC)
 *
 * - Detects an optical drive on the USB Host port, waits for an audio CD
 * - Reads TOC and CD-TEXT, starts playing (autoplay setting), streams CD-DA to the I2S DAC (player.c)
 * - Shows track info / transport state on the display (ui.c)
 * - Controlled via serial commands on the console UART (type 'h' for help);
 *   physical buttons will be added later
 * - Wi-Fi with firmware update over HTTP (net.c, ota.c, tools/ota.py); LittleFS at /data (storage.c)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "usb/usb_host.h"
#include "usb/atapi_host.h"
#include "player.h"
#include "audio_out.h"
#include "ui.h"
#include "net.h"
#include "ota.h"
#include "storage.h"
#include "app.h"
#include "meta.h"
#include "auth.h"

static const char *TAG = "cd";

#define CD_READ_SPEED_KBPS  (4 * ATAPI_SPEED_1X_KBPS)  /* 4x: plenty of margin, less noise than max */
#define TRANSPORT_FAILURES_HUNG 2 /* consecutive polls failing at the USB/BOT level (each ~10 s): drive hung */

typedef enum {
    APP_DEVICE_CONNECTED,
    APP_DEVICE_DISCONNECTED,
    APP_EJECT_TOGGLE,
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
/* USB                                                                      */
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
/* Input: serial commands                                                   */
/* ------------------------------------------------------------------------ */

void app_eject_toggle(void)
{
    app_msg_t msg = {.id = APP_EJECT_TOGGLE};
    xQueueSend(s_queue, &msg, 0);
}

static const char *state_name(player_state_t s)
{
    static const char *names[] = {"no disc", "stopped", "playing", "paused"};
    return names[s];
}

static void print_status(void)
{
    player_status_t st;
    player_get_status(&st);
    uint32_t pos = st.track_pos / ATAPI_CD_FRAMES_PER_SECOND, len = st.track_len / ATAPI_CD_FRAMES_PER_SECOND;
    printf("STATUS state=%s track=%d/%d pos=%02"PRIu32":%02"PRIu32"/%02"PRIu32":%02"PRIu32" vol=%d buf=%d%%(%"PRIu32"ms) "
           "underruns=%"PRIu32" read_errors=%"PRIu32"%s title=\"%s\"%s%s\n",
           state_name(st.state), st.track, st.num_tracks, pos / 60, pos % 60, len / 60, len % 60, player_volume_display(st.volume),
           st.buffer_pct, st.buffer_ms, st.underruns, st.read_errors, st.deemphasis ? " deemph" : "",
           st.state == PLAYER_NO_DISC ? st.message : st.title,
           st.performer[0] ? " by " : "", st.performer);
}

static void print_help(void)
{
    printf("Commands: p=play/pause s=stop n=next b=previous f/r=+/-10s +/-=volume e=eject/load\n"
           "          <number><Enter>=play track, i=status, h=help\n"
           "          a=check I2S pins, t=1 kHz test tone (3 s)\n"
           "          w=network/firmware info, W=set Wi-Fi credentials, M=look up album info online again\n"
           "          P=clear the web UI admin password, E=force de-emphasis on/off (test)\n");
}

/**
 * Read a line with echo (Enter terminates; a LF right after CR is skipped, so CRLF terminals work).
 * @param mask echo '*' instead of the characters
 */
static void read_line(uart_port_t port, char *buf, size_t size, bool mask)
{
    static bool last_cr;
    size_t len = 0;
    while (true) {
        uint8_t ch;
        if (uart_read_bytes(port, &ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch == '\n' && last_cr) {
            last_cr = false;
            continue;
        }
        last_cr = ch == '\r';
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if ((ch == '\b' || ch == 0x7f) && len > 0) {
            len--;
            printf("\b \b");
        } else if (ch >= 0x20 && ch < 0x7f && len < size - 1) {
            buf[len++] = ch;
            putchar(mask ? '*' : ch);
        }
        fflush(stdout);
    }
    buf[len] = '\0';
    printf("\n");
}

static void configure_wifi(uart_port_t port)
{
    char ssid[33], pass[65];
    printf("Wi-Fi SSID (empty = clear): ");
    fflush(stdout);
    read_line(port, ssid, sizeof(ssid), false);
    pass[0] = '\0';
    if (ssid[0]) {
        printf("Password (empty = open network): ");
        fflush(stdout);
        read_line(port, pass, sizeof(pass), true);
    }
    esp_err_t err = net_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        printf("Saving Wi-Fi credentials failed: %s\n", esp_err_to_name(err));
    }
}

/** Simple command interface on the console UART, so the player can be driven without buttons */
static void console_task(void *arg)
{
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    ESP_ERROR_CHECK(uart_driver_install(port, 256, 0, 0, NULL, 0));
    uart_set_pin(port, CONFIG_ESP_CONSOLE_UART_TX_GPIO, CONFIG_ESP_CONSOLE_UART_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    print_help();

    int number = -1;
    while (true) {
        uint8_t ch;
        if (uart_read_bytes(port, &ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (isdigit(ch)) {
            number = (number < 0 ? 0 : number * 10) + (ch - '0');
            continue;
        }
        switch (ch) {
        case '\r':
        case '\n':
            if (number > 0) {
                player_command(PLAYER_CMD_TRACK, number);
            }
            break;
        case 'p': player_command(PLAYER_CMD_PLAY_PAUSE, 0); break;
        case 's': player_command(PLAYER_CMD_STOP, 0); break;
        case 'n': player_command(PLAYER_CMD_NEXT, 0); break;
        case 'b': player_command(PLAYER_CMD_PREV, 0); break;
        case 'f': player_command(PLAYER_CMD_SEEK_REL, 10); break;
        case 'r': player_command(PLAYER_CMD_SEEK_REL, -10); break;
        case '+': player_command(PLAYER_CMD_VOLUME_REL, PLAYER_VOLUME_STEP); break;
        case '-': player_command(PLAYER_CMD_VOLUME_REL, -PLAYER_VOLUME_STEP); break;
        case 'e': app_eject_toggle(); break;
        case 'a': audio_out_check_pins(); break;
        case 't': {
            player_status_t st;
            player_get_status(&st);
            if (st.state == PLAYER_PLAYING) {
                player_command(PLAYER_CMD_PAUSE, 0); /* don't write to I2S from two tasks */
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            printf("Test tone 1 kHz, 3 s\n");
            audio_out_test_tone(1000, 3000);
            break;
        }
        case 'i': break;
        case 'w': net_print_info(); meta_print_info(); number = -1; continue;
        case 'M': meta_refresh(); number = -1; continue;
        case 'E': player_force_deemphasis(!player_deemphasis_forced()); break;
        case 'P':
            printf("Admin password %s\n", auth_set_password("") == ESP_OK ? "cleared" : "NOT cleared (NVS error)");
            number = -1;
            continue;
        case 'W': configure_wifi(port); number = -1; continue;
        case 'h': print_help(); break;
        default:
            number = -1; /* unknown character: discard a partially typed track number */
            continue;
        }
        number = -1;
        vTaskDelay(pdMS_TO_TICKS(50)); /* let the player apply the command */
        print_status();
    }
}

/* ------------------------------------------------------------------------ */
/* Disc handling                                                            */
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
            printf(" - %s", perf);
        }
        if (atapi_toc_entry_has_preemphasis(t)) {
            printf("  (pre-emphasis)");
        }
        printf("\n");
    }
    printf("\n");
}

static bool load_disc(atapi_host_device_handle_t dev)
{
    static atapi_toc_t toc; /* ~1 KB */
    player_set_message("Reading disc...");
    esp_err_t err = atapi_host_read_toc(dev, &toc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "READ TOC failed: %s (0x%x)", esp_err_to_name(err), err);
        player_set_message("Cannot read disc");
        return false;
    }

    atapi_cd_text_t *cdtext = calloc(1, sizeof(*cdtext));
    err = cdtext ? atapi_host_read_cd_text(dev, cdtext) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "READ CD-TEXT failed: %s (0x%x)", esp_err_to_name(err), err);
        }
        free(cdtext);
        cdtext = NULL;
    }
    print_toc(&toc, cdtext);

    bool has_audio = false;
    for (int i = 0; i < toc.num_tracks; i++) {
        has_audio |= !atapi_toc_entry_is_data(&toc.tracks[i]);
    }
    if (!has_audio) {
        /* Data-only disc: nothing to play, no metadata lookup. The player stays without a disc (so the UI shows
         * the message), but the disc counts as loaded: it isn't re-read on every poll, and eject still works. */
        ESP_LOGI(TAG, "No audio tracks on this disc");
        if (cdtext) {
            atapi_cd_text_free(cdtext);
            free(cdtext);
        }
        player_set_message("No audio tracks");
        return true;
    }

    err = atapi_host_set_speed(dev, CD_READ_SPEED_KBPS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SET CD SPEED not accepted (%s), using drive default", esp_err_to_name(err));
    }
    bool has_cdtext = cdtext != NULL;
    player_load_disc(&toc, cdtext); /* player owns cdtext now */
    meta_disc_loaded(&toc, has_cdtext); /* cached / online album info and cover */
    if (player_get_autoplay()) { /* start as soon as a disc is loaded (inserted, or present at boot) */
        player_command(PLAYER_CMD_PLAY, 0);
        ESP_LOGI(TAG, "Autoplay");
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Drive state (main task only)                                             */
/* ------------------------------------------------------------------------ */

#define MSG_DRIVE_HUNG "Drive not responding - power-cycle it"

static struct {
    atapi_host_device_handle_t dev;
    bool disc_loaded;
    bool tray_open;
    bool locked;              /* PREVENT MEDIUM REMOVAL active (only with GESN, so the button still works) */
    int gesn;                 /* GET EVENT STATUS NOTIFICATION: -1 unknown, 0 not supported, 1 supported */
    esp_err_t last_state;
    int transport_failures;   /* consecutive polls that failed at the USB/BOT level */
} D;

static void unload(void)
{
    player_unload_disc();
    meta_disc_unloaded();
    D.disc_loaded = false;
}

static void unlock_tray(void)
{
    if (D.locked) {
        atapi_host_prevent_removal(D.dev, false);
        D.locked = false;
    }
}

static void eject_toggle(void)
{
    D.tray_open = !D.tray_open;
    ESP_LOGI(TAG, "%s tray", D.tray_open ? "Ejecting" : "Loading");
    if (D.tray_open) {
        unload();
        unlock_tray();
        player_set_message("Ejecting...");
    } else {
        player_set_message("Loading...");
    }
    esp_err_t err = D.tray_open ? atapi_host_eject(D.dev) : atapi_host_load(D.dev);
    if (err != ESP_OK) {
        atapi_sense_t sense;
        atapi_host_get_last_sense(D.dev, &sense);
        ESP_LOGW(TAG, "Tray command failed: %s (0x%x), sense %X/%02X/%02X (%s)", esp_err_to_name(err), err,
                 sense.key, sense.asc, sense.ascq, atapi_sense_to_str(&sense));
    }
    D.last_state = ESP_FAIL; /* refresh the message on the next poll */
}

/**
 * Disc state for the 1 s poll. With GESN (media event status), a loaded disc costs one short command and the
 * drive isn't disturbed; it also reports the tray state and eject button presses. Without GESN: TEST UNIT READY.
 * @return ESP_OK (disc ready / still loaded), ESP_ERR_ATAPI_NO_MEDIUM, ESP_ERR_ATAPI_NOT_READY or an error
 */
static esp_err_t poll_drive(bool *eject_request, bool *media_changed)
{
    if (D.gesn != 0) {
        atapi_media_status_t ms;
        esp_err_t err = atapi_host_get_media_status(D.dev, &ms);
        if (err == ESP_OK) {
            if (D.gesn < 0) {
                ESP_LOGI(TAG, "Drive reports media events (GESN): tray state, eject button");
                D.gesn = 1;
            }
            D.tray_open = ms.tray_open;
            *eject_request = ms.event == ATAPI_MEDIA_EVENT_EJECT_REQUEST;
            *media_changed = ms.event == ATAPI_MEDIA_EVENT_NEW_MEDIA || ms.event == ATAPI_MEDIA_EVENT_MEDIA_CHANGED;
            if (!ms.media_present) {
                return ESP_ERR_ATAPI_NO_MEDIUM;
            }
            if (D.disc_loaded && !*media_changed) {
                return ESP_OK; /* still there; no TEST UNIT READY needed (also fine while spun down) */
            }
            return atapi_host_wait_ready(D.dev, 1000); /* present: readable yet? */
        }
        if (err == ESP_ERR_ATAPI_TRANSPORT || err == ESP_ERR_INVALID_STATE) {
            return err;
        }
        if (D.gesn < 0) {
            ESP_LOGI(TAG, "No media events (GESN: %s), polling TEST UNIT READY", esp_err_to_name(err));
        }
        D.gesn = 0;
    }
    esp_err_t state = atapi_host_wait_ready(D.dev, 1000);
    if (state == ESP_ERR_ATAPI_NOT_READY && D.disc_loaded && player_is_spun_down()) {
        return ESP_OK; /* motor stopped for power saving, the disc is still there */
    }
    return state;
}

/* ------------------------------------------------------------------------ */

void app_main(void)
{
    s_queue = xQueueCreate(8, sizeof(app_msg_t));
    assert(s_queue);

    /* NVS holds the persistent settings (volume) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing (%s), settings are reset", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ota_log_boot_info();
    storage_init(); /* not fatal: the player works without the cache */

    ESP_ERROR_CHECK(player_init());
    ESP_ERROR_CHECK(meta_init());
    ESP_ERROR_CHECK(ui_init());
    /* After the UI, so the LVGL DMA buffers get internal RAM first. Not fatal: the player works offline */
    auth_init();
    err = net_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
    }
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "Waiting for USB optical drive...");

    while (true) {
        app_msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (msg.id) {
            case APP_DEVICE_CONNECTED: {
                if (D.dev) {
                    ESP_LOGW(TAG, "Only one drive supported, ignoring device %d", msg.address);
                    break;
                }
                player_set_message("Drive found...");
                atapi_host_device_handle_t dev;
                esp_err_t err = atapi_host_install_device(msg.address, &dev);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Device %d is not a usable optical drive: %s", msg.address, esp_err_to_name(err));
                    /* NOT_SUPPORTED: not a BOT CD/DVD device. Otherwise INQUIRY never got an answer: hung drive */
                    player_set_message(err == ESP_ERR_NOT_SUPPORTED ? "Unsupported USB device" : MSG_DRIVE_HUNG);
                    break;
                }
                atapi_host_device_info_t info;
                atapi_host_get_device_info(dev, &info);
                ESP_LOGI(TAG, "Optical drive connected: %s %s (rev %s), VID 0x%04x PID 0x%04x",
                         info.vendor, info.product, info.revision, info.vid, info.pid);
                memset(&D, 0, sizeof(D));
                D.dev = dev;
                D.gesn = -1;
                D.last_state = ESP_FAIL;
                player_set_device(dev);
                break;
            }
            case APP_DEVICE_DISCONNECTED:
                if (D.dev && msg.handle == D.dev) {
                    ESP_LOGI(TAG, "Optical drive disconnected");
                    unload();
                    player_set_device(NULL);
                    atapi_host_uninstall_device(D.dev);
                    D.dev = NULL;
                    player_set_message("Waiting for drive");
                }
                break;
            case APP_EJECT_TOGGLE:
                if (D.dev) {
                    eject_toggle();
                }
                break;
            }
            continue;
        }

        /* Periodic disc presence polling */
        if (!D.dev) {
            continue;
        }
        bool eject_request = false, media_changed = false;
        esp_err_t state = poll_drive(&eject_request, &media_changed);

        /* A hung drive: the bridge takes commands but never answers, BOT reset recovery doesn't help, and
         * neither does a USB bus reset (tested: root port power cycle / ESP reboot re-enumerate the bridge,
         * the drive behind it stays hung). Only cutting the drive's power helps; VBUS isn't switchable on
         * the T-Display S3, so tell the user. */
        D.transport_failures = state == ESP_ERR_ATAPI_TRANSPORT ? D.transport_failures + 1 : 0;
        if (D.transport_failures == TRANSPORT_FAILURES_HUNG) {
            ESP_LOGE(TAG, "Drive not responding (%d failed polls); it needs a power cycle", D.transport_failures);
            player_set_message(MSG_DRIVE_HUNG);
        }
        if (eject_request && !D.tray_open) {
            ESP_LOGI(TAG, "Eject button pressed");
            eject_toggle();
            continue;
        }
        if (media_changed && D.disc_loaded) {
            ESP_LOGI(TAG, "Disc changed");
            unload();
        }
        if (state != D.last_state) {
            if (state == ESP_ERR_ATAPI_NO_MEDIUM) {
                atapi_sense_t s;
                atapi_host_get_last_sense(D.dev, &s);
                /* With GESN, poll_drive() already knows the tray state. Otherwise the sense says it: ASCQ 01 = tray
                 * closed, 02 = open. Some drives (JLMS XJ-HD165H) only report the generic 3A/00: then keep the
                 * tracked state (otherwise eject would never close the tray again). */
                if (D.gesn <= 0 && (s.ascq == 0x01 || s.ascq == 0x02)) {
                    D.tray_open = s.ascq == 0x02;
                }
                ESP_LOGI(TAG, "No disc, tray %s", D.tray_open ? "open" : "closed");
                player_set_message(D.tray_open ? "Tray open" : "No disc");
            } else if (state == ESP_ERR_ATAPI_NOT_READY) {
                ESP_LOGI(TAG, "Disc becoming ready...");
                player_set_message("Loading...");
            } else if (state != ESP_OK) {
                ESP_LOGW(TAG, "Drive state: %s (0x%x)", esp_err_to_name(state), state);
                if (D.transport_failures < TRANSPORT_FAILURES_HUNG) {
                    player_set_message("Drive error");
                }
            }
            D.last_state = state;
        }
        if (state == ESP_OK && !D.disc_loaded) {
            D.tray_open = false;
            ESP_LOGI(TAG, "Disc ready");
            D.disc_loaded = load_disc(D.dev);
            if (D.disc_loaded && D.gesn > 0) {
                /* The eject button now only reports an eject request (GESN), so playback stops cleanly first */
                D.locked = atapi_host_prevent_removal(D.dev, true) == ESP_OK;
                ESP_LOGI(TAG, "Tray %slocked while the disc is loaded", D.locked ? "" : "NOT ");
            }
        } else if (state != ESP_OK && D.disc_loaded) {
            ESP_LOGI(TAG, "Disc removed");
            unload();
        }
    }
}
