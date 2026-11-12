/*
 * Settings / status menu on the device screen: the web UI's settings and information
 *
 * Model: pages of items described by tables (an info line, a switch, a choice, a stepper over a value range or
 * table, an action with an optional confirmation, a text field), with callbacks into the player / net / meta /
 * backlight modules. The renderer below turns them into LVGL rows and refreshes them every 200 ms, so changes
 * made elsewhere (web UI) show up. Nothing here is board specific; it only depends on:
 *   - the screen size class (UI_LAYOUT_LARGE): two panes (pages left, items right) and large touch targets, or
 *     one pane (page list, then a page with a "Back" row) and small fonts;
 *   - text input (UI_MENU_TEXT_INPUT = touch screen, on-screen keyboard). Without it, items that need typing
 *     are left out (hostname, admin password, password of a secured Wi-Fi network): the web UI has them.
 * Every control is added to the menu's lv_group (ui_menu_group()): a keypad / encoder input device (physical
 * buttons) bound to it moves the focus between the controls, ENTER activates.
 *
 * Settings changed here aren't protected by the web UI's admin password: physical access already allows
 * clearing it (serial command 'P').
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "board.h"
#include "player.h"
#include "net.h"
#include "ota.h"
#include "meta.h"
#include "storage.h"
#include "auth.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "ui_menu";

#define UI_MENU_TEXT_INPUT BOARD_HAS_TOUCH

#if UI_LAYOUT_LARGE
#define F_TEXT     (&font_text_28)
#define F_SMALL    (&font_text_20)
#define PAD        24
#define ROW_H      76      /* min. row height */
#define CTL_H      60      /* buttons, choices */
#define LABEL_W    300     /* item name column */
#define STEP_BTN_W 72
#define VALUE_W    180     /* stepper value */
#define TWO_PANE   1
#define SIDE_W     280     /* page list */
#define HDR_H      64      /* header: title, message, close button */
#define KB_H       330     /* on-screen keyboard */
#else
#define F_TEXT     (&font_text_14)
#define F_SMALL    (&font_text_14)
#define PAD        6
#define ROW_H      28
#define CTL_H      22
#define LABEL_W    110
#define STEP_BTN_W 26
#define VALUE_W    70
#define TWO_PANE   0
#define HDR_H      24
#define KB_H       0
#endif

#define SCR_W BOARD_LCD_H_RES
#define SCR_H BOARD_LCD_V_RES

#define COLOR_ROW   lv_color_hex(0x141414)
#define COLOR_BTN   lv_color_hex(0x262626)
#define COLOR_PRESS lv_color_hex(0x3c3c3c)
#define COLOR_SEL   lv_color_hex(0x10324a)
#define COLOR_WARN  lv_color_hex(0xe0a020)

#define MAX_ROWS     32
#define MAX_NETWORKS 16
#define MSG_MS       4000  /* header messages ("Saved", errors) */

/* ------------------------------------------------------------------------ */
/* Model                                                                    */
/* ------------------------------------------------------------------------ */

typedef enum {
    MI_HEADING,  /* section title */
    MI_INFO,     /* label: value (text) */
    MI_TOGGLE,   /* switch (get/set 0/1) */
    MI_CHOICE,   /* one of choices[] (get/set index) */
    MI_STEP,     /* - value + over steps[] or min..max (get/set value, fmt) */
    MI_ACTION,   /* button (action, optional confirm; text = dynamic button label) */
    MI_TEXT,     /* label: value [Edit] -> keyboard (text = current value, set_text); needs UI_MENU_TEXT_INPUT */
} mi_type_t;

typedef struct {
    mi_type_t type;
    const char *label;
    bool (*visible)(void);                       /* NULL = always */
    void (*text)(char *buf, size_t size);
    int (*get)(void);
    void (*set)(int v);
    const char *const *choices;                  /* MI_CHOICE, "" terminated (button matrix map) */
    const int *steps;                            /* MI_STEP: ascending values, or NULL for min..max */
    int n_steps, min, max;
    void (*fmt)(int v, char *buf, size_t size);  /* MI_STEP */
    void (*action)(void);                        /* MI_ACTION */
    const char *confirm;                         /* MI_ACTION: question asked first, NULL = none */
    esp_err_t (*set_text)(const char *s);        /* MI_TEXT */
    int max_len;                                 /* MI_TEXT */
} mi_t;

typedef struct {
    const char *title;
    const mi_t *items;
    int n_items;
} page_t;

static void menu_message(const char *msg, bool error);
static void open_scan(void);
static void reboot_now(void);

/* ---- Player ---- */

static int get_autoplay(void) { return player_get_autoplay(); }
static void set_autoplay(int v) { player_set_autoplay(v != 0); }

static const mi_t s_player_items[] = {
    {.type = MI_TOGGLE, .label = "Autoplay", .get = get_autoplay, .set = set_autoplay},
};

/* ---- Display (backlight) ---- */

static const char *const s_dim_modes[] = {"Never", "When idle", "Always", ""}; /* = ui_dim_mode_t */
static const int s_dim_delays[] = {5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
static const int s_off_delays[] = {0, 60, 120, 300, 600, 900, 1800, 3600, 7200, 14400, 28800, 86400};

#define BL_FIELD(name, field)                                            \
    static int get_##name(void)                                          \
    {                                                                    \
        ui_backlight_cfg_t c;                                            \
        ui_get_backlight(&c);                                            \
        return (int)c.field;                                             \
    }                                                                    \
    static void set_##name(int v)                                        \
    {                                                                    \
        ui_backlight_cfg_t c;                                            \
        ui_get_backlight(&c);                                            \
        c.field = v;                                                     \
        if (ui_set_backlight(&c) != ESP_OK) {                            \
            menu_message("Applied, but saving failed", true);           \
        }                                                                \
    }
BL_FIELD(dim_mode, mode)
BL_FIELD(bl_on, on_level)
BL_FIELD(bl_dim, dim_level)
BL_FIELD(bl_dim_delay, dim_delay_s)
BL_FIELD(bl_off_delay, off_delay_s)

static void fmt_level(int v, char *b, size_t n) { v ? snprintf(b, n, "%d", v) : snprintf(b, n, "Off"); }

static void fmt_duration(int s, char *b, size_t n)
{
    if (s >= 3600 && s % 3600 == 0) {
        snprintf(b, n, "%d h", s / 3600);
    } else if (s >= 60 && s % 60 == 0) {
        snprintf(b, n, "%d min", s / 60);
    } else {
        snprintf(b, n, "%d s", s);
    }
}

static void fmt_off_delay(int s, char *b, size_t n) { s ? fmt_duration(s, b, n) : (void)snprintf(b, n, "Never"); }

static const mi_t s_display_items[] = {
    {.type = MI_CHOICE, .label = "Dimming", .choices = s_dim_modes, .get = get_dim_mode, .set = set_dim_mode},
    {.type = MI_STEP, .label = "Brightness", .min = 1, .max = BOARD_BRIGHTNESS_MAX, .fmt = fmt_level,
     .get = get_bl_on, .set = set_bl_on},
    {.type = MI_STEP, .label = "Dimmed", .min = 0, .max = BOARD_BRIGHTNESS_MAX, .fmt = fmt_level,
     .get = get_bl_dim, .set = set_bl_dim},
    {.type = MI_STEP, .label = "Dim after", .steps = s_dim_delays, .n_steps = sizeof(s_dim_delays) / sizeof(int),
     .fmt = fmt_duration, .get = get_bl_dim_delay, .set = set_bl_dim_delay},
    {.type = MI_STEP, .label = "Off when idle", .steps = s_off_delays, .n_steps = sizeof(s_off_delays) / sizeof(int),
     .fmt = fmt_off_delay, .get = get_bl_off_delay, .set = set_bl_off_delay},
};

/* ---- Network ---- */

static net_status_t net(void)
{
    net_status_t s;
    net_get_status(&s);
    return s;
}

#if BOARD_HAS_ETH
static void txt_eth_state(char *b, size_t n)
{
    net_status_t s = net();
    if (!s.eth_present) {
        snprintf(b, n, "Not available");
    } else if (s.eth_connected) {
        snprintf(b, n, "Connected, %d Mbps %s duplex", s.eth_speed_mbps, s.eth_full_duplex ? "full" : "half");
    } else {
        snprintf(b, n, s.eth_link ? "Link up, waiting for an address" : "No cable / no link");
    }
}

static void txt_eth_ip(char *b, size_t n)
{
    net_status_t s = net();
    snprintf(b, n, "%s", s.eth_ip[0] ? s.eth_ip : "-");
}
#endif

static void txt_wifi_ssid(char *b, size_t n)
{
    net_status_t s = net();
    snprintf(b, n, "%s", s.configured ? s.ssid : "Not configured");
}

static void txt_wifi_state(char *b, size_t n)
{
    net_status_t s = net();
    snprintf(b, n, "%s", !s.configured ? "-" : s.connected ? "Connected" : "Connecting...");
}

static void txt_wifi_ip(char *b, size_t n)
{
    net_status_t s = net();
    snprintf(b, n, "%s", s.ip[0] ? s.ip : "-");
}

static void txt_wifi_rssi(char *b, size_t n)
{
    wifi_ap_record_t ap;
    if (net().connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(b, n, "%d dBm", ap.rssi);
    } else {
        snprintf(b, n, "-");
    }
}

static void txt_hostname(char *b, size_t n)
{
    net_status_t s = net();
    snprintf(b, n, "%s.local", s.hostname);
}

static int get_ap(void) { return net().ap_active; }
static void set_ap(int v) { net_setup_ap(v != 0); }
static bool ap_on(void) { return net().ap_active; }

static void txt_ap(char *b, size_t n)
{
    net_status_t s = net();
    const char *pw = net_ap_password();
    snprintf(b, n, "%s, password %s, %d client(s), http://" NET_AP_IP_STR, s.ap_ssid, pw[0] ? pw : "none",
             s.ap_clients);
}

#if UI_MENU_TEXT_INPUT
static void txt_hostname_raw(char *b, size_t n)
{
    net_status_t s = net();
    snprintf(b, n, "%s", s.hostname);
}

static esp_err_t set_hostname(const char *name)
{
    esp_err_t err = net_set_hostname(name);
    if (err == ESP_ERR_INVALID_ARG) {
        menu_message("Use letters, digits and '-' (not at the start or end)", true);
    } else {
        menu_message(err == ESP_OK ? "Hostname saved" : "Saving failed", err != ESP_OK);
    }
    return err;
}
#endif

static const mi_t s_network_items[] = {
#if BOARD_HAS_ETH
    {.type = MI_HEADING, .label = "Ethernet"},
    {.type = MI_INFO, .label = "Status", .text = txt_eth_state},
    {.type = MI_INFO, .label = "Address", .text = txt_eth_ip},
#endif
    {.type = MI_HEADING, .label = "Wi-Fi"},
    {.type = MI_INFO, .label = "Network", .text = txt_wifi_ssid},
    {.type = MI_INFO, .label = "Status", .text = txt_wifi_state},
    {.type = MI_INFO, .label = "Address", .text = txt_wifi_ip},
    {.type = MI_INFO, .label = "Signal", .text = txt_wifi_rssi},
    {.type = MI_ACTION, .label = "Choose a network", .action = open_scan},
    {.type = MI_TOGGLE, .label = "Setup access point", .get = get_ap, .set = set_ap},
    {.type = MI_INFO, .label = "Access point", .text = txt_ap, .visible = ap_on},
    {.type = MI_HEADING, .label = "Name"},
#if UI_MENU_TEXT_INPUT
    {.type = MI_TEXT, .label = "Hostname", .text = txt_hostname_raw, .set_text = set_hostname,
     .max_len = NET_HOSTNAME_MAX},
#endif
    {.type = MI_INFO, .label = "Web UI", .text = txt_hostname},
};

/* ---- Album info ---- */

static meta_info_t meta(void)
{
    meta_info_t m;
    meta_get_info(&m);
    return m;
}

static void txt_meta_src(char *b, size_t n)
{
    meta_info_t m = meta();
    snprintf(b, n, "%s%s%s", m.discid[0] ? meta_state_name(m.state) : "No disc", m.error[0] ? ": " : "", m.error);
}

static void txt_meta_id(char *b, size_t n)
{
    meta_info_t m = meta();
    snprintf(b, n, "%s", m.discid[0] ? m.discid : "-");
}

static void txt_meta_rel(char *b, size_t n)
{
    meta_info_t m = meta();
    snprintf(b, n, "%s", m.release[0] ? m.release : "-");
}

static bool has_disc(void) { return meta().discid[0] != '\0'; }

static bool has_cached(void)
{
    meta_info_t m = meta();
    return m.state == META_FROM_CACHE || m.state == META_FOUND;
}

/** Storage mounted: entries of other discs can be there even without a disc loaded */
static bool has_storage(void)
{
    size_t total = 0, used = 0;
    storage_get_info(&total, &used);
    return total != 0;
}

static void do_meta_refresh(void)
{
    meta_refresh();
    menu_message("Looking up...", false);
}

static void do_meta_forget(void)
{
    meta_forget();
    menu_message("Cached album info deleted", false);
}

static void do_meta_forget_all(void)
{
    meta_forget_all();
    menu_message("Whole cache deleted", false);
}

static const mi_t s_album_items[] = {
    {.type = MI_INFO, .label = "Source", .text = txt_meta_src},
    {.type = MI_INFO, .label = "Disc ID", .text = txt_meta_id},
    {.type = MI_INFO, .label = "Release", .text = txt_meta_rel},
    {.type = MI_ACTION, .label = "Look up online again", .action = do_meta_refresh, .visible = has_disc},
    {.type = MI_ACTION, .label = "Delete the cached info", .action = do_meta_forget, .visible = has_cached,
     .confirm = "Delete this disc's cached album info and cover?"},
    {.type = MI_ACTION, .label = "Delete the whole cache", .action = do_meta_forget_all, .visible = has_storage,
     .confirm = "Delete the stored album info and covers of ALL discs, not just this one? Export them first in "
                "the web UI. This cannot be undone."},
};

/* ---- Status ---- */

static player_status_t pst(void)
{
    player_status_t st;
    player_get_status(&st);
    return st;
}

static void txt_buffer(char *b, size_t n)
{
    player_status_t st = pst();
    if (st.state == PLAYER_NO_DISC) {
        snprintf(b, n, "-");
    } else {
        snprintf(b, n, "%d %% (%.1f s)", st.buffer_pct, st.buffer_ms / 1000.0);
    }
}

static void txt_underruns(char *b, size_t n) { snprintf(b, n, "%"PRIu32, pst().underruns); }
static void txt_read_errors(char *b, size_t n) { snprintf(b, n, "%"PRIu32, pst().read_errors); }

static void txt_ram(char *b, size_t n)
{
    snprintf(b, n, "%u KiB free (min %u KiB)", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

static void txt_psram(char *b, size_t n)
{
    snprintf(b, n, "%u KiB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void txt_storage(char *b, size_t n)
{
    size_t total = 0, used = 0;
    storage_get_info(&total, &used);
    if (total) {
        snprintf(b, n, "%u KiB used of %u KiB", (unsigned)(used / 1024), (unsigned)(total / 1024));
    } else {
        snprintf(b, n, "Not mounted");
    }
}

static void txt_uptime(char *b, size_t n)
{
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s >= 86400) {
        snprintf(b, n, "%"PRIu32" d %02"PRIu32":%02"PRIu32":%02"PRIu32, s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
    } else {
        snprintf(b, n, "%02"PRIu32":%02"PRIu32":%02"PRIu32, s / 3600, s / 60 % 60, s % 60);
    }
}

static const mi_t s_status_items[] = {
    {.type = MI_HEADING, .label = "Playback"},
    {.type = MI_INFO, .label = "Buffer", .text = txt_buffer},
    {.type = MI_INFO, .label = "Underruns", .text = txt_underruns},
    {.type = MI_INFO, .label = "Read errors", .text = txt_read_errors},
    {.type = MI_HEADING, .label = "Memory"},
    {.type = MI_INFO, .label = "Internal RAM", .text = txt_ram},
    {.type = MI_INFO, .label = "PSRAM", .text = txt_psram},
    {.type = MI_INFO, .label = "Storage", .text = txt_storage},
    {.type = MI_INFO, .label = "Uptime", .text = txt_uptime},
};

/* ---- System ---- */

static void txt_fw_version(char *b, size_t n) { snprintf(b, n, "%s", esp_app_get_description()->version); }

static void txt_fw_built(char *b, size_t n)
{
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(b, n, "%s %s", app->date, app->time);
}

static void txt_fw_part(char *b, size_t n)
{
    snprintf(b, n, "%s (%s)", esp_ota_get_running_partition()->label, ota_running_state_name());
}

static void txt_reset(char *b, size_t n) { snprintf(b, n, "%s", ota_reset_reason_name()); }
static void txt_idf(char *b, size_t n) { snprintf(b, n, "%s", esp_app_get_description()->idf_ver); }
static void txt_board(char *b, size_t n) { snprintf(b, n, "%s", BOARD_NAME); }
static void txt_admin(char *b, size_t n) { snprintf(b, n, "%s", auth_enabled() ? "Set" : "Not set"); }

static void do_admin_clear(void)
{
    menu_message(auth_set_password("") == ESP_OK ? "Admin password removed" : "Saving failed", false);
}

#if UI_MENU_TEXT_INPUT
static void txt_empty(char *b, size_t n) { b[0] = '\0'; }

static esp_err_t set_admin(const char *pw)
{
    if (!pw[0]) {
        return ESP_OK; /* "Remove" does that */
    }
    esp_err_t err = auth_set_password(pw);
    menu_message(err == ESP_OK ? "Admin password set" : "Saving failed", err != ESP_OK);
    return err;
}
#endif

static const mi_t s_system_items[] = {
    {.type = MI_HEADING, .label = "Firmware"},
    {.type = MI_INFO, .label = "Version", .text = txt_fw_version},
    {.type = MI_INFO, .label = "Built", .text = txt_fw_built},
    {.type = MI_INFO, .label = "Partition", .text = txt_fw_part},
    {.type = MI_INFO, .label = "Last reset", .text = txt_reset},
    {.type = MI_INFO, .label = "ESP-IDF", .text = txt_idf},
    {.type = MI_INFO, .label = "Board", .text = txt_board},
    {.type = MI_ACTION, .label = "Reboot", .action = reboot_now, .confirm = "Reboot the player now?"},
    {.type = MI_HEADING, .label = "Web UI admin password"},
    {.type = MI_INFO, .label = "Password", .text = txt_admin},
#if UI_MENU_TEXT_INPUT
    {.type = MI_TEXT, .label = "New password", .text = txt_empty, .set_text = set_admin, .max_len = AUTH_PASSWORD_MAX},
#endif
    {.type = MI_ACTION, .label = "Remove the password", .action = do_admin_clear, .visible = auth_enabled,
     .confirm = "Remove the admin password? Anyone on the network can then change settings."},
};

#define PAGE(t, list) {.title = t, .items = list, .n_items = sizeof(list) / sizeof(list[0])}
static const page_t s_pages[] = {
    PAGE("Player", s_player_items),
    PAGE("Display", s_display_items),
    PAGE("Network", s_network_items),
    PAGE("Album info", s_album_items),
    PAGE("Status", s_status_items),
    PAGE("System", s_system_items),
};
#define NUM_PAGES ((int)(sizeof(s_pages) / sizeof(s_pages[0])))
#define PAGE_NETWORK 2

/* ------------------------------------------------------------------------ */
/* Renderer                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    const mi_t *mi;
    lv_obj_t *row;
    lv_obj_t *value;  /* MI_INFO / MI_STEP / MI_TEXT value label, MI_ACTION button label */
    lv_obj_t *ctl;    /* switch, button matrix */
    int shown;        /* MI_CHOICE / MI_TOGGLE: value shown, -1 = none yet */
} row_t;

static lv_obj_t *s_root, *s_title, *s_msg, *s_back, *s_list;
static lv_obj_t *s_side_btn[NUM_PAGES];
static lv_group_t *s_group;
static row_t s_rows[MAX_ROWS];
static int s_nrows;
static int s_page = -1;          /* shown page, -1 = page list (one-pane) or the Wi-Fi scan */
static int64_t s_msg_until;

/* Wi-Fi scan (runs in its own task: it blocks ~2 s) */
static net_scan_entry_t s_nets[MAX_NETWORKS];
static volatile int s_nets_n;
static volatile bool s_scanning, s_scan_done;
static bool s_scan_shown;        /* the scan page is shown */
static char s_pick_ssid[33];     /* network chosen, waiting for its password */

/* Overlays: confirmation, text entry */
static lv_obj_t *s_dialog;
static void (*s_confirm_cb)(void);
#if UI_MENU_TEXT_INPUT
static lv_obj_t *s_kb_panel, *s_kb_title, *s_kb_ta, *s_kb;
static esp_err_t (*s_kb_done)(const char *text);
#endif

static void show_page(int page);

static void style_btn(lv_obj_t *b)
{
    lv_obj_set_style_bg_color(b, COLOR_BTN, 0);
    lv_obj_set_style_bg_color(b, COLOR_PRESS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, COLOR_SEL, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_radius(b, UI_LAYOUT_LARGE ? 14 : 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY); /* button navigation: show the focus */
    lv_obj_set_style_outline_color(b, UI_COLOR_ACCENT, LV_STATE_FOCUS_KEY);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *arg, lv_obj_t **label)
{
    lv_obj_t *b = lv_button_create(parent);
    style_btn(b);
    lv_obj_set_height(b, CTL_H);
    lv_obj_set_style_pad_hor(b, UI_LAYOUT_LARGE ? 24 : 6, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, F_TEXT, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    lv_group_add_obj(s_group, b);
    if (label) {
        *label = l;
    }
    return b;
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    return p;
}

static void menu_message(const char *msg, bool error)
{
    if (!s_msg) {
        return;
    }
    lv_obj_set_style_text_color(s_msg, error ? COLOR_WARN : UI_COLOR_ACCENT, 0);
    lv_label_set_text(s_msg, msg);
    s_msg_until = esp_timer_get_time() + (int64_t)MSG_MS * 1000;
}

/* ---- Rows ---- */

static void clear_rows(void)
{
    lv_obj_clean(s_list); /* also removes them from the group */
    s_nrows = 0;
}

/** Row: name on the left, the control on the right (grows with wrapped text) */
static lv_obj_t *new_row(void)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, ROW_H, 0);
    lv_obj_set_style_bg_color(row, COLOR_ROW, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, UI_LAYOUT_LARGE ? 10 : 3, 0);
    lv_obj_set_style_pad_hor(row, UI_LAYOUT_LARGE ? 16 : 4, 0);
    lv_obj_set_style_pad_ver(row, UI_LAYOUT_LARGE ? 8 : 2, 0);
    lv_obj_set_style_pad_column(row, UI_LAYOUT_LARGE ? 16 : 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static lv_obj_t *row_label(lv_obj_t *row, const char *text, int w, lv_color_t color)
{
    lv_obj_t *l = ui_make_label(row, F_TEXT, color, 0, 0, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, text);
    return l;
}

/** Value label taking the rest of the row */
static lv_obj_t *row_value(lv_obj_t *row)
{
    lv_obj_t *v = ui_make_label(row, F_TEXT, lv_color_white(), 0, 0, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    return v;
}

static row_t *row_of(lv_event_t *e)
{
    return &s_rows[(int)(intptr_t)lv_event_get_user_data(e)];
}

static void toggle_cb(lv_event_t *e)
{
    row_t *r = row_of(e);
    int v = lv_obj_has_state(r->ctl, LV_STATE_CHECKED);
    r->shown = v;
    r->mi->set(v);
}

static void choice_cb(lv_event_t *e)
{
    row_t *r = row_of(e);
    uint32_t i = lv_buttonmatrix_get_selected_button(r->ctl);
    if (i != LV_BUTTONMATRIX_BUTTON_NONE && (int)i != r->shown) {
        r->shown = (int)i;
        r->mi->set((int)i);
    }
}

/** Next / previous value of a stepper (dir +1 / -1): the table entry beyond the current value, or +-1 */
static int step_value(const mi_t *mi, int cur, int dir)
{
    if (!mi->steps) {
        int v = cur + dir;
        return v < mi->min ? mi->min : v > mi->max ? mi->max : v;
    }
    if (dir > 0) {
        for (int i = 0; i < mi->n_steps; i++) {
            if (mi->steps[i] > cur) {
                return mi->steps[i];
            }
        }
        return mi->steps[mi->n_steps - 1];
    }
    for (int i = mi->n_steps - 1; i >= 0; i--) {
        if (mi->steps[i] < cur) {
            return mi->steps[i];
        }
    }
    return mi->steps[0];
}

static void step_cb(lv_event_t *e, int dir)
{
    if (!ui_released_inside(e)) {
        return;
    }
    row_t *r = row_of(e);
    int cur = r->mi->get();
    int v = step_value(r->mi, cur, dir);
    if (v != cur) {
        r->mi->set(v);
    }
}

static void step_down_cb(lv_event_t *e) { step_cb(e, -1); }
static void step_up_cb(lv_event_t *e) { step_cb(e, +1); }

static void confirm_open(const char *question, const char *ok, void (*cb)(void));

static void action_cb(lv_event_t *e)
{
    if (!ui_released_inside(e)) {
        return;
    }
    const mi_t *mi = row_of(e)->mi;
    if (mi->confirm) {
        confirm_open(mi->confirm, mi->label, mi->action);
    } else {
        mi->action();
    }
}

#if UI_MENU_TEXT_INPUT
static void text_open(const char *title, const char *initial, int max_len, esp_err_t (*done)(const char *));

static void text_cb(lv_event_t *e)
{
    if (!ui_released_inside(e)) {
        return;
    }
    const mi_t *mi = row_of(e)->mi;
    char cur[96];
    mi->text(cur, sizeof(cur));
    text_open(mi->label, cur, mi->max_len, mi->set_text);
}
#endif

static void add_row(const mi_t *mi)
{
    if (s_nrows >= MAX_ROWS) {
        ESP_LOGE(TAG, "Too many rows");
        return;
    }
    int idx = s_nrows++;
    row_t *r = &s_rows[idx];
    *r = (row_t){.mi = mi, .shown = -1};
    void *arg = (void *)(intptr_t)idx;
    r->row = new_row();

    switch (mi->type) {
    case MI_HEADING:
        lv_obj_set_style_bg_opa(r->row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_min_height(r->row, 0, 0);
        lv_obj_set_style_pad_top(r->row, UI_LAYOUT_LARGE ? 16 : 4, 0);
        row_label(r->row, mi->label, 0, UI_COLOR_ACCENT);
        break;
    case MI_INFO:
        row_label(r->row, mi->label, LABEL_W, UI_COLOR_DIM);
        r->value = row_value(r->row);
        break;
    case MI_TOGGLE:
        lv_obj_set_flex_grow(row_label(r->row, mi->label, 0, lv_color_white()), 1);
        r->ctl = lv_switch_create(r->row);
        lv_obj_set_size(r->ctl, UI_LAYOUT_LARGE ? 100 : 36, UI_LAYOUT_LARGE ? 50 : 18);
        lv_obj_set_style_bg_color(r->ctl, UI_COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_outline_width(r->ctl, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_color(r->ctl, UI_COLOR_ACCENT, LV_STATE_FOCUS_KEY);
        lv_obj_set_ext_click_area(r->ctl, 16);
        lv_obj_add_event_cb(r->ctl, toggle_cb, LV_EVENT_VALUE_CHANGED, arg);
        lv_group_add_obj(s_group, r->ctl);
        break;
    case MI_CHOICE: {
        /* Label, then the choices on their own full-width line (they don't fit next to the label) */
        lv_obj_set_flex_flow(r->row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(r->row, UI_LAYOUT_LARGE ? 10 : 3, 0);
        lv_obj_set_style_pad_ver(r->row, UI_LAYOUT_LARGE ? 14 : 3, 0);
        row_label(r->row, mi->label, 0, lv_color_white());
        r->ctl = lv_buttonmatrix_create(r->row);
        lv_buttonmatrix_set_map(r->ctl, mi->choices);
        lv_buttonmatrix_set_button_ctrl_all(r->ctl, LV_BUTTONMATRIX_CTRL_CHECKABLE);
        lv_buttonmatrix_set_one_checked(r->ctl, true);
        lv_obj_set_width(r->ctl, lv_pct(100));
        lv_obj_set_height(r->ctl, CTL_H);
        lv_obj_set_style_text_font(r->ctl, F_TEXT, 0);
        lv_obj_set_style_bg_opa(r->ctl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r->ctl, 0, 0);
        lv_obj_set_style_pad_all(r->ctl, 0, 0);
        lv_obj_set_style_pad_column(r->ctl, UI_LAYOUT_LARGE ? 10 : 3, 0);
        lv_obj_set_style_bg_color(r->ctl, COLOR_BTN, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(r->ctl, COLOR_PRESS, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(r->ctl, UI_COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(r->ctl, lv_color_white(), LV_PART_ITEMS);
        lv_obj_set_style_radius(r->ctl, UI_LAYOUT_LARGE ? 14 : 4, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(r->ctl, 0, LV_PART_ITEMS);
        lv_obj_set_style_border_width(r->ctl, 0, LV_PART_ITEMS);
        lv_obj_add_event_cb(r->ctl, choice_cb, LV_EVENT_VALUE_CHANGED, arg);
        lv_group_add_obj(s_group, r->ctl);
        break;
    }
    case MI_STEP: {
        lv_obj_set_flex_grow(row_label(r->row, mi->label, 0, lv_color_white()), 1);
        lv_obj_t *b = make_btn(r->row, LV_SYMBOL_MINUS, step_down_cb, arg, NULL);
        lv_obj_set_width(b, STEP_BTN_W);
        r->value = ui_make_label(r->row, F_TEXT, lv_color_white(), 0, 0, VALUE_W);
        lv_obj_set_style_text_align(r->value, LV_TEXT_ALIGN_CENTER, 0);
        b = make_btn(r->row, LV_SYMBOL_PLUS, step_up_cb, arg, NULL);
        lv_obj_set_width(b, STEP_BTN_W);
        break;
    }
    case MI_ACTION:
        make_btn(r->row, mi->label, action_cb, arg, &r->value);
        break;
    case MI_TEXT:
#if UI_MENU_TEXT_INPUT
        row_label(r->row, mi->label, LABEL_W, UI_COLOR_DIM);
        r->value = row_value(r->row);
        make_btn(r->row, "Change", text_cb, arg, NULL);
#endif
        break;
    }
}

/** Refresh a row from the model (skips controls the user is operating) */
static void sync_row(row_t *r)
{
    const mi_t *mi = r->mi;
    ui_set_hidden(r->row, mi->visible && !mi->visible());
    char buf[160];
    switch (mi->type) {
    case MI_INFO:
    case MI_TEXT:
        if (r->value && mi->text) {
            mi->text(buf, sizeof(buf));
            ui_set_text_utf8(r->value, buf[0] ? buf : (mi->type == MI_TEXT ? "" : "-"));
        }
        break;
    case MI_TOGGLE: {
        int v = mi->get() != 0;
        if (v != r->shown && !lv_obj_has_state(r->ctl, LV_STATE_PRESSED)) {
            r->shown = v;
            if (v) {
                lv_obj_add_state(r->ctl, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(r->ctl, LV_STATE_CHECKED);
            }
        }
        break;
    }
    case MI_CHOICE: {
        int v = mi->get();
        if (v != r->shown) {
            r->shown = v;
            lv_buttonmatrix_set_button_ctrl(r->ctl, (uint32_t)v, LV_BUTTONMATRIX_CTRL_CHECKED); /* one checked */
        }
        break;
    }
    case MI_STEP:
        mi->fmt(mi->get(), buf, sizeof(buf));
        ui_set_text(r->value, buf);
        break;
    default:
        break;
    }
}

/* ---- Pages ---- */

static void side_cb(lv_event_t *e)
{
    if (ui_released_inside(e)) {
        show_page((int)(intptr_t)lv_event_get_user_data(e));
    }
}

static void set_title(const char *title)
{
    lv_label_set_text(s_title, title);
    ui_set_hidden(s_back, TWO_PANE || (s_page < 0 && !s_scan_shown));
}

static void begin_page(int page)
{
    s_page = page;
    s_scan_shown = false;
    clear_rows();
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    for (int i = 0; i < NUM_PAGES && TWO_PANE; i++) {
        if (i == page) {
            lv_obj_add_state(s_side_btn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_side_btn[i], LV_STATE_CHECKED);
        }
    }
}

/** One-pane: the list of pages */
static void show_index(void)
{
    begin_page(-1);
    set_title("Settings");
    for (int i = 0; i < NUM_PAGES; i++) {
        lv_obj_t *b = make_btn(s_list, s_pages[i].title, side_cb, (void *)(intptr_t)i, NULL);
        lv_obj_set_width(b, lv_pct(100));
    }
}

static void show_page(int page)
{
    begin_page(page);
    set_title(s_pages[page].title);
    for (int i = 0; i < s_pages[page].n_items; i++) {
        add_row(&s_pages[page].items[i]);
    }
    for (int i = 0; i < s_nrows; i++) {
        sync_row(&s_rows[i]);
    }
}

/* ---- Wi-Fi network list ---- */

static void scan_task(void *arg)
{
    int n = net_scan(s_nets, MAX_NETWORKS);
    s_nets_n = n;
    s_scan_done = true;
    s_scanning = false;
    vTaskDelete(NULL);
}

static void scan_start(void)
{
    if (s_scanning) {
        return;
    }
    s_scan_done = false;
    s_scanning = true;
    if (xTaskCreate(scan_task, "ui_scan", 4096, NULL, 3, NULL) != pdPASS) {
        s_scanning = false;
        menu_message("Scan failed (no memory)", true);
    }
}

#if UI_MENU_TEXT_INPUT
static esp_err_t wifi_password_done(const char *pw)
{
    if (strlen(pw) > 63) {
        menu_message("The password is too long (max. 63)", true);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = net_set_credentials(s_pick_ssid, pw);
    if (err == ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to %s", s_pick_ssid);
        menu_message(msg, false);
        show_page(PAGE_NETWORK);
    } else {
        menu_message("Saving failed", true);
    }
    return err;
}
#endif

static void network_cb(lv_event_t *e)
{
    if (!ui_released_inside(e)) {
        return;
    }
    const net_scan_entry_t *n = &s_nets[(int)(intptr_t)lv_event_get_user_data(e)];
    strlcpy(s_pick_ssid, n->ssid, sizeof(s_pick_ssid));
    if (n->open) {
        net_set_credentials(s_pick_ssid, "");
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to %s", s_pick_ssid);
        menu_message(msg, false);
        show_page(PAGE_NETWORK);
        return;
    }
#if UI_MENU_TEXT_INPUT
    char title[64];
    snprintf(title, sizeof(title), "Password for %s", s_pick_ssid);
    text_open(title, "", 63, wifi_password_done);
#else
    menu_message("Secured network: enter its password in the web UI", true);
#endif
}

static void rescan_cb(lv_event_t *e)
{
    if (ui_released_inside(e)) {
        open_scan();
    }
}

static void show_scan_results(void)
{
    clear_rows();
    lv_obj_t *b = make_btn(s_list, LV_SYMBOL_REFRESH "  Scan again", rescan_cb, NULL, NULL);
    lv_obj_set_width(b, lv_pct(100));
    if (s_nets_n <= 0) {
        lv_obj_t *row = new_row();
        row_label(row, s_nets_n < 0 ? "Scan failed" : "No networks found", 0, UI_COLOR_DIM);
        return;
    }
    char text[64];
    for (int i = 0; i < s_nets_n; i++) {
        lv_obj_t *nb = make_btn(s_list, "", network_cb, (void *)(intptr_t)i, NULL);
        lv_obj_set_width(nb, lv_pct(100));
        lv_obj_set_style_min_height(nb, CTL_H, 0);
        lv_obj_set_height(nb, LV_SIZE_CONTENT);
        lv_obj_t *name = lv_obj_get_child(nb, 0);
        lv_obj_set_width(name, lv_pct(70));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        ui_set_text_utf8(name, s_nets[i].ssid);
        snprintf(text, sizeof(text), "%d dBm%s", s_nets[i].rssi, s_nets[i].open ? "  open" : "");
        lv_obj_t *info = ui_make_label(nb, F_SMALL, UI_COLOR_DIM, 0, 0, 0);
        lv_label_set_text(info, text);
        lv_obj_align(info, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void open_scan(void)
{
    begin_page(-1);
    s_scan_shown = true;
    if (TWO_PANE) {
        lv_obj_add_state(s_side_btn[PAGE_NETWORK], LV_STATE_CHECKED);
    }
    set_title("Wi-Fi networks");
    lv_obj_t *row = new_row();
    row_label(row, "Scanning...", 0, UI_COLOR_DIM);
    scan_start();
}

/* ---- Confirmation dialog ---- */

static void dialog_close(void)
{
    if (s_dialog) {
        lv_obj_delete(s_dialog);
        s_dialog = NULL;
    }
}

static void dialog_cb(lv_event_t *e)
{
    if (!ui_released_inside(e)) {
        return;
    }
    void (*cb)(void) = lv_event_get_user_data(e) ? s_confirm_cb : NULL;
    dialog_close();
    if (cb) {
        cb();
    }
}

static void confirm_open(const char *question, const char *ok, void (*cb)(void))
{
    dialog_close();
    s_confirm_cb = cb;
    s_dialog = lv_obj_create(s_root); /* dims and blocks everything behind it */
    lv_obj_set_size(s_dialog, SCR_W, SCR_H);
    lv_obj_set_pos(s_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_dialog, 0, 0);
    lv_obj_set_style_radius(s_dialog, 0, 0);
    lv_obj_remove_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_dialog);
    lv_obj_set_size(box, UI_LAYOUT_LARGE ? 600 : SCR_W - 20, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1c1c1c), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, UI_LAYOUT_LARGE ? 18 : 6, 0);
    lv_obj_set_style_pad_all(box, UI_LAYOUT_LARGE ? 28 : 8, 0);
    lv_obj_set_style_pad_row(box, UI_LAYOUT_LARGE ? 24 : 6, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *q = ui_make_label(box, F_TEXT, lv_color_white(), 0, 0, 0);
    lv_obj_set_width(q, lv_pct(100));
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(q, question);

    lv_obj_t *btns = make_panel(box);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, UI_LAYOUT_LARGE ? 16 : 6, 0);
    make_btn(btns, "Cancel", dialog_cb, NULL, NULL);
    lv_obj_t *okb = make_btn(btns, ok, dialog_cb, (void *)1, NULL);
    lv_obj_set_style_bg_color(okb, lv_color_hex(0x1b5a85), 0);
    lv_group_focus_obj(okb);
}

/* ---- Text entry (on-screen keyboard) ---- */

#if UI_MENU_TEXT_INPUT
static void text_close(void)
{
    lv_obj_add_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    s_kb_done = NULL;
}

static void kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        esp_err_t (*done)(const char *) = s_kb_done;
        if (done && done(lv_textarea_get_text(s_kb_ta)) != ESP_OK) {
            return; /* stays open; the message says why */
        }
    }
    text_close();
}

static void kb_cancel_cb(lv_event_t *e)
{
    if (ui_released_inside(e)) {
        text_close();
    }
}

static void text_open(const char *title, const char *initial, int max_len, esp_err_t (*done)(const char *))
{
    s_kb_done = done;
    lv_label_set_text(s_kb_title, title);
    lv_textarea_set_max_length(s_kb_ta, max_len);
    lv_textarea_set_text(s_kb_ta, initial);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb_panel);
}

static void build_keyboard(void)
{
    s_kb_panel = lv_obj_create(s_root);
    lv_obj_set_size(s_kb_panel, SCR_W, SCR_H);
    lv_obj_set_pos(s_kb_panel, 0, 0);
    lv_obj_set_style_bg_color(s_kb_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_kb_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb_panel, 0, 0);
    lv_obj_set_style_radius(s_kb_panel, 0, 0);
    lv_obj_set_style_pad_all(s_kb_panel, 0, 0);
    lv_obj_remove_flag(s_kb_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_kb_title = ui_make_label(s_kb_panel, F_TEXT, UI_COLOR_DIM, PAD, PAD + 14, SCR_W - 2 * PAD - 200);
    lv_label_set_long_mode(s_kb_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *cancel = make_btn(s_kb_panel, "Cancel", kb_cancel_cb, NULL, NULL);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -PAD, PAD);

    s_kb_ta = lv_textarea_create(s_kb_panel);
    lv_textarea_set_one_line(s_kb_ta, true);
    lv_obj_set_pos(s_kb_ta, PAD, PAD + HDR_H + 16);
    lv_obj_set_width(s_kb_ta, SCR_W - 2 * PAD);
    lv_obj_set_style_text_font(s_kb_ta, F_TEXT, 0);
    lv_obj_set_style_bg_color(s_kb_ta, COLOR_ROW, 0);
    lv_obj_set_style_text_color(s_kb_ta, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_kb_ta, UI_COLOR_ACCENT, 0);
    lv_obj_add_state(s_kb_ta, LV_STATE_FOCUSED); /* cursor visible */

    s_kb = lv_keyboard_create(s_kb_panel);
    lv_obj_set_size(s_kb, SCR_W, KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, F_TEXT, 0);
    lv_keyboard_set_textarea(s_kb, s_kb_ta);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
}
#endif

/* ---- Reboot ---- */

static void reboot_timer_cb(lv_timer_t *t)
{
    ESP_LOGI(TAG, "Reboot requested from the screen");
    esp_restart();
}

static void reboot_now(void)
{
    menu_message("Rebooting...", false);
    lv_timer_t *t = lv_timer_create(reboot_timer_cb, 400, NULL); /* the message shows first */
    lv_timer_set_repeat_count(t, 1);
}

/* ------------------------------------------------------------------------ */
/* Public                                                                   */
/* ------------------------------------------------------------------------ */

static void close_cb(lv_event_t *e)
{
    if (ui_released_inside(e)) {
        ui_menu_close();
    }
}

static void back_cb(lv_event_t *e)
{
    if (!ui_released_inside(e)) {
        return;
    }
    if (s_scan_shown) {
        show_page(PAGE_NETWORK);
    } else {
        show_index();
    }
}

void ui_menu_build(lv_obj_t *scr)
{
    s_group = lv_group_create();

    s_root = lv_obj_create(scr); /* opaque and clickable: nothing reaches the player screen behind it */
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    int cx = PAD; /* content column */
#if TWO_PANE
    lv_obj_t *s_side = make_panel(s_root);
    lv_obj_set_pos(s_side, PAD, PAD);
    lv_obj_set_size(s_side, SIDE_W, SCR_H - 2 * PAD);
    lv_obj_set_flex_flow(s_side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_side, 10, 0);
    lv_obj_t *hdr = ui_make_label(s_side, &font_text_28, UI_COLOR_DIM, 0, 0, 0);
    lv_obj_set_style_pad_ver(hdr, 14, 0);
    lv_label_set_text(hdr, LV_SYMBOL_SETTINGS "  Settings");
    for (int i = 0; i < NUM_PAGES; i++) {
        s_side_btn[i] = make_btn(s_side, s_pages[i].title, side_cb, (void *)(intptr_t)i, NULL);
        lv_obj_set_size(s_side_btn[i], lv_pct(100), CTL_H);
        lv_obj_align(lv_obj_get_child(s_side_btn[i], 0), LV_ALIGN_LEFT_MID, 0, 0);
    }
    cx = PAD + SIDE_W + PAD;
#endif
    const int cw = SCR_W - cx - PAD;

    /* Header: [back] title  message  [close] */
    s_back = make_btn(s_root, LV_SYMBOL_LEFT, back_cb, NULL, NULL);
    lv_obj_set_pos(s_back, cx, PAD);
    lv_obj_set_size(s_back, HDR_H, HDR_H);
    s_title = ui_make_label(s_root, &font_text_28, lv_color_white(), cx, PAD + (HDR_H - 32) / 2, 0);
#if !UI_LAYOUT_LARGE
    lv_obj_set_style_text_font(s_title, F_TEXT, 0);
    lv_obj_set_y(s_title, PAD + 4);
#endif
    s_msg = ui_make_label(s_root, F_SMALL, UI_COLOR_ACCENT, 0, 0, cw - HDR_H - 220);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_msg, LV_ALIGN_TOP_RIGHT, -(PAD + HDR_H + 16), PAD + (HDR_H - 24) / 2);
    lv_obj_t *close = make_btn(s_root, LV_SYMBOL_CLOSE, close_cb, NULL, NULL);
    lv_obj_set_size(close, HDR_H, HDR_H);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -PAD, PAD);

    s_list = make_panel(s_root);
    lv_obj_set_pos(s_list, cx, PAD + HDR_H + 12);
    lv_obj_set_size(s_list, cw, SCR_H - PAD - HDR_H - 12 - PAD);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, UI_LAYOUT_LARGE ? 8 : 3, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);

#if UI_MENU_TEXT_INPUT
    build_keyboard();
#endif
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void ui_menu_open(void)
{
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    lv_label_set_text(s_msg, "");
    if (TWO_PANE) {
        show_page(s_page >= 0 ? s_page : 0); /* where it was left */
    } else {
        show_index();
    }
}

void ui_menu_close(void)
{
    dialog_close();
#if UI_MENU_TEXT_INPUT
    text_close();
#endif
    clear_rows(); /* rebuilt on the next open */
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

bool ui_menu_is_open(void)
{
    return s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

lv_group_t *ui_menu_group(void)
{
    return s_group;
}

void ui_menu_update(void)
{
    if (!ui_menu_is_open()) {
        return;
    }
    if (s_msg_until && esp_timer_get_time() > s_msg_until) {
        s_msg_until = 0;
        lv_label_set_text(s_msg, "");
    }
    if (s_scan_shown && s_scan_done) {
        s_scan_done = false;
        show_scan_results();
    }
    for (int i = 0; i < s_nrows; i++) {
        sync_row(&s_rows[i]);
    }
}
