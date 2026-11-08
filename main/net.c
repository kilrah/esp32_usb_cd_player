/*
 * Wi-Fi station + mDNS
 *
 * Credentials are stored in NVS (namespace "wifi"), set with the serial command 'W'. Without stored
 * credentials, the Kconfig defaults (CONFIG_CD_WIFI_SSID/PASSWORD) are used. On the first IP address,
 * the HTTP server for OTA updates is started (ota.c), which also confirms a freshly updated firmware.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "nvs.h"
#include "net.h"
#include "ota.h"
#include "storage.h"

static const char *TAG = "net";

#define NVS_NAMESPACE        "wifi"
#define RECONNECT_MIN_MS     1000
#define RECONNECT_MAX_MS     30000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static net_status_t s_status;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;

static void load_credentials(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t h;
    bool found = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(cfg->sta.ssid);
        if (nvs_get_str(h, "ssid", (char *)cfg->sta.ssid, &len) == ESP_OK) {
            len = sizeof(cfg->sta.password);
            if (nvs_get_str(h, "pass", (char *)cfg->sta.password, &len) != ESP_OK) {
                cfg->sta.password[0] = '\0';
            }
            found = true;
        }
        nvs_close(h);
    }
    if (!found) {
        strlcpy((char *)cfg->sta.ssid, CONFIG_CD_WIFI_SSID, sizeof(cfg->sta.ssid));
        strlcpy((char *)cfg->sta.password, CONFIG_CD_WIFI_PASSWORD, sizeof(cfg->sta.password));
    }
    /* Accept open networks too (threshold OPEN); WPA3 SAE where the AP offers it */
    cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    portENTER_CRITICAL(&s_lock);
    s_status.configured = cfg->sta.ssid[0] != '\0';
    strlcpy(s_status.ssid, (const char *)cfg->sta.ssid, sizeof(s_status.ssid));
    portEXIT_CRITICAL(&s_lock);
}

static void schedule_reconnect(uint32_t delay_ms)
{
    esp_timer_stop(s_reconnect_timer); /* fails harmlessly if not running */
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
}

static void reconnect_cb(void *arg)
{
    if (s_status.configured) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_status.configured) {
            ESP_LOGI(TAG, "Connecting to \"%s\"...", s_status.ssid);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "No Wi-Fi credentials; set them with the serial command 'W'");
            ota_confirm_running_app("no Wi-Fi configured");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        bool was_connected = s_status.connected;
        portENTER_CRITICAL(&s_lock);
        s_status.connected = false;
        s_status.ip[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        if (!s_status.configured) {
            return;
        }
        if (was_connected) {
            ESP_LOGW(TAG, "Disconnected (reason %d), reconnecting", ev->reason);
            s_backoff_ms = RECONNECT_MIN_MS;
        } else {
            ESP_LOGI(TAG, "Connect to \"%s\" failed (reason %d), retry in %"PRIu32" s",
                     s_status.ssid, ev->reason, s_backoff_ms / 1000);
        }
        schedule_reconnect(s_backoff_ms);
        s_backoff_ms = MIN(s_backoff_ms * 2, RECONNECT_MAX_MS);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        portENTER_CRITICAL(&s_lock);
        s_status.connected = true;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        portEXIT_CRITICAL(&s_lock);
        s_backoff_ms = RECONNECT_MIN_MS;
        ESP_LOGI(TAG, "Connected to \"%s\", IP %s, http://%s.local/", s_status.ssid, s_status.ip,
                 CONFIG_CD_HOSTNAME);
        if (ota_server_start() == ESP_OK) {
            ota_confirm_running_app("OTA server reachable");
        }
    }
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(CONFIG_CD_HOSTNAME);
        mdns_instance_name_set("USB CD player");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    }
}

esp_err_t net_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, CONFIG_CD_HOSTNAME);

    const esp_timer_create_args_t timer_args = {.callback = reconnect_cb, .name = "wifi_reconn"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "timer");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    esp_log_level_set("wifi", ESP_LOG_WARN); /* we log connect/disconnect ourselves */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage"); /* credentials live in our own namespace */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    wifi_config_t cfg;
    load_credentials(&cfg);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    /* Mains powered: no modem sleep, for responsive HTTP and fast OTA uploads */
    esp_wifi_set_ps(WIFI_PS_NONE);
    start_mdns();
    return ESP_OK;
}

esp_err_t net_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err;
    if (ssid[0]) {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "pass", password);
        }
    } else {
        err = nvs_erase_all(h);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write");

    wifi_config_t cfg;
    load_credentials(&cfg); /* an empty NVS SSID falls back to the Kconfig default */
    esp_timer_stop(s_reconnect_timer);
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
    s_backoff_ms = RECONNECT_MIN_MS;
    if (s_status.configured) {
        ESP_LOGI(TAG, "Connecting to \"%s\"...", s_status.ssid);
        schedule_reconnect(500);
    } else {
        ESP_LOGI(TAG, "Wi-Fi credentials cleared");
    }
    return ESP_OK;
}

void net_get_status(net_status_t *st)
{
    portENTER_CRITICAL(&s_lock);
    *st = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void net_print_info(void)
{
    net_status_t st;
    net_get_status(&st);
    if (!st.configured) {
        printf("WIFI not configured (W to set)\n");
    } else if (st.connected) {
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        printf("WIFI connected ssid=\"%s\" ip=%s rssi=%d dBm url=http://%s.local/\n",
               st.ssid, st.ip, rssi, CONFIG_CD_HOSTNAME);
    } else {
        printf("WIFI connecting ssid=\"%s\"\n", st.ssid);
    }
    ota_print_info();
    storage_print_info();
    printf("HEAP internal free=%u (min %u) psram free=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
