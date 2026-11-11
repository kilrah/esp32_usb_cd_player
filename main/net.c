/*
 * Wi-Fi station + setup access point + mDNS
 *
 * Credentials are stored in NVS (namespace "wifi"), set with the serial command 'W' or the web UI. Without
 * stored credentials, the Kconfig defaults (CONFIG_CD_WIFI_SSID/PASSWORD) are used.
 * The hostname (DHCP, mDNS <hostname>.local) is CONFIG_CD_HOSTNAME unless one is set in the web UI (NVS
 * namespace "net", key "hostname"; a separate namespace, since clearing the credentials erases "wifi").
 *
 * Setup access point "CDPlayer-XXXX" (192.168.4.1, WPA2 with CONFIG_CD_AP_PASSWORD): started right away when
 * no network is configured, or AP_START_DELAY_MS after the station lost / failed to get a connection. The
 * station keeps retrying in the background (AP+STA). A captive-portal DNS makes phones open the web UI.
 * Once the station has an IP address again, the AP is switched off AP_STOP_DELAY_MS later (so a setup page
 * can still show the new address). AP and STA share the radio and a connection attempt scans all channels,
 * which disturbs AP clients: while someone is connected to the AP, attempts are limited to one per
 * AP_CLIENT_RETRY_MS (except right after new credentials were saved).
 *
 * Ethernet (boards with BOARD_HAS_ETH, driver from board_eth_init()): DHCP, same hostname, preferred over the
 * station for outgoing traffic (route priority). Both can be connected at once; the web UI answers on both.
 * The setup AP only comes on while neither has an IP address: without Wi-Fi credentials it waits
 * ETH_AP_GRACE_MS for Ethernet first, and an Ethernet connection switches it off like a station connection.
 *
 * The HTTP server (ota.c) starts with the first IP address on any interface.
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
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "dhcpserver/dhcpserver.h"
#include "mdns.h"
#include "nvs.h"
#include "captive_dns.h"
#include "net.h"
#include "ota.h"
#include "storage.h"
#include "board.h"
#if BOARD_HAS_ETH
#include "esp_eth.h"
#endif

static const char *TAG = "net";

#define NVS_NAMESPACE        "wifi"
#define NVS_NS_NET           "net"   /* hostname */
#define RECONNECT_MIN_MS     1000
#define RECONNECT_MAX_MS     30000
#define AP_START_DELAY_MS    30000  /* configured network not reachable for this long -> setup AP */
#define AP_STOP_DELAY_MS     30000  /* station connected for this long -> setup AP off */
#define AP_CLIENT_RETRY_MS   60000  /* min. interval of station attempts while the AP has clients */
#define AP_MAX_CLIENTS       4
#define ETH_AP_GRACE_MS      10000  /* no Wi-Fi credentials: time Ethernet gets to connect before the setup AP */
#define ETH_ROUTE_PRIO       150    /* above the Wi-Fi station's 100: Ethernet is the default route */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static net_status_t s_status;
static esp_netif_t *s_netif, *s_ap_netif;
static esp_timer_handle_t s_reconnect_timer, s_ap_start_timer, s_ap_stop_timer;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;
static int64_t s_last_attempt_us;
static bool s_force_attempt;   /* next attempt ignores the AP-client limit (new credentials) */
#if BOARD_HAS_ETH
static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth;
#endif

/** An IP address on the station or on Ethernet (the setup AP isn't needed) */
static bool any_connected(void)
{
    return s_status.connected || s_status.eth_connected;
}

/** A valid DNS label: 1..NET_HOSTNAME_MAX letters, digits and '-', not starting or ending with '-' */
static bool hostname_valid(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > NET_HOSTNAME_MAX || n[0] == '-' || n[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static void load_hostname(void)
{
    char name[NET_HOSTNAME_MAX + 1] = "";
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NET, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(name);
        if (nvs_get_str(h, "hostname", name, &len) != ESP_OK || !hostname_valid(name)) {
            name[0] = '\0';
        }
        nvs_close(h);
    }
    strlcpy(s_status.hostname, name[0] ? name : CONFIG_CD_HOSTNAME, sizeof(s_status.hostname));
}

esp_err_t net_set_hostname(const char *name)
{
    if (name[0] && !hostname_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    char n[NET_HOSTNAME_MAX + 1];
    strlcpy(n, name[0] ? name : CONFIG_CD_HOSTNAME, sizeof(n));
    for (char *c = n; *c; c++) {
        *c = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c; /* mDNS is case-insensitive anyway */
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_NET, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = name[0] ? nvs_set_str(h, "hostname", n) : nvs_erase_key(h, "hostname");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* already at the default */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write");

    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.hostname, n, sizeof(s_status.hostname));
    portEXIT_CRITICAL(&s_lock);
    esp_netif_set_hostname(s_netif, n); /* sent with the next DHCP request */
#if BOARD_HAS_ETH
    if (s_eth_netif) {
        esp_netif_set_hostname(s_eth_netif, n);
    }
#endif
    mdns_hostname_set(n);
    ESP_LOGI(TAG, "Hostname %s: http://%s.local/", name[0] ? "set" : "back to the default", n);
    return ESP_OK;
}

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

static void timer_restart(esp_timer_handle_t t, uint32_t delay_ms)
{
    esp_timer_stop(t); /* fails harmlessly if not running */
    esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
}

/* ------------------------------------------------------------------------ */
/* Setup access point                                                       */
/* ------------------------------------------------------------------------ */

const char *net_ap_password(void)
{
    return strlen(CONFIG_CD_AP_PASSWORD) >= 8 ? CONFIG_CD_AP_PASSWORD : "";
}

static void ap_start(void)
{
    if (s_status.ap_active) {
        return;
    }
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_status.ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_status.ap_ssid);
    strlcpy((char *)ap.ap.password, net_ap_password(), sizeof(ap.ap.password));
    ap.ap.authmode = ap.ap.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.channel = 1;  /* follows the station's channel once that connects */
    ap.ap.max_connection = AP_MAX_CLIENTS;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "Starting the setup AP failed");
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.ap_active = true;
    s_status.ap_clients = 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "Setup AP \"%s\" (%s) on, web UI at http://" NET_AP_IP_STR "/", s_status.ap_ssid,
             net_ap_password()[0] ? "WPA2" : "open");
    captive_dns_start(ESP_IP4TOADDR(192, 168, 4, 1));
    ota_server_start(); /* the web UI must be reachable without a station connection */
}

static void ap_stop(void)
{
    if (!s_status.ap_active) {
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    portENTER_CRITICAL(&s_lock);
    s_status.ap_active = false;
    s_status.ap_clients = 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Setup AP off");
}

static void ap_start_cb(void *arg)
{
    if (any_connected()) {
        return;
    }
    if (s_status.configured) {
        ESP_LOGW(TAG, "No connection to \"%s\"%s", s_status.ssid, s_status.eth_present ? " or Ethernet" : "");
    } else {
        ESP_LOGW(TAG, "No Wi-Fi credentials and no Ethernet connection");
    }
    ap_start();
}

static void ap_arm_start_timer(void);

void net_setup_ap(bool on)
{
    esp_timer_stop(s_ap_stop_timer);
    if (on) {
        ap_start();
    } else {
        ap_stop();
        if (!any_connected() && s_status.configured) {
            ap_arm_start_timer(); /* still no connection: it comes back after the usual delay */
        }
    }
}

static void ap_stop_cb(void *arg)
{
    if (any_connected()) {
        ap_stop();
    }
}

/** Arm the "no connection -> setup AP" timer (unless the AP is on or the timer already runs) */
static void ap_arm_start_timer(void)
{
    if (!s_status.ap_active && !esp_timer_is_active(s_ap_start_timer)) {
        esp_timer_start_once(s_ap_start_timer, (uint64_t)AP_START_DELAY_MS * 1000);
    }
}

/** DHCP on the AP offers 192.168.4.1 as DNS server (the captive DNS). Only the AP's DHCP server is affected. */
static void ap_netif_setup(void)
{
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1)};
    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dhcps_start(s_ap_netif);
}

/* ------------------------------------------------------------------------ */
/* Station                                                                  */
/* ------------------------------------------------------------------------ */

static void reconnect_cb(void *arg)
{
    if (!s_status.configured) {
        return;
    }
    int64_t now = esp_timer_get_time();
    int64_t wait_us = s_last_attempt_us + (int64_t)AP_CLIENT_RETRY_MS * 1000 - now;
    if (s_status.ap_clients > 0 && !s_force_attempt && wait_us > 0) {
        /* Don't disturb setup clients with channel scans too often */
        timer_restart(s_reconnect_timer, (uint32_t)(wait_us / 1000) + 1);
        return;
    }
    s_force_attempt = false;
    s_last_attempt_us = now;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_status.configured) {
            ESP_LOGI(TAG, "Connecting to \"%s\"...", s_status.ssid);
            s_last_attempt_us = esp_timer_get_time();
            esp_wifi_connect();
            ap_arm_start_timer();
        } else {
            ESP_LOGW(TAG, "No Wi-Fi credentials; set them in the web UI (setup AP) or with the serial command 'W'");
            ota_confirm_running_app("no Wi-Fi configured");
            if (s_status.eth_present) {
                if (!any_connected() && !s_status.ap_active) {
                    timer_restart(s_ap_start_timer, ETH_AP_GRACE_MS); /* unless Ethernet connects first */
                }
            } else {
                ap_start();
            }
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
            if (!any_connected()) { /* still on Ethernet: the setup AP may still go off */
                esp_timer_stop(s_ap_stop_timer);
            }
        } else {
            ESP_LOGI(TAG, "Connect to \"%s\" failed (reason %d), retry in %"PRIu32" s",
                     s_status.ssid, ev->reason, s_backoff_ms / 1000);
        }
        ap_arm_start_timer();
        timer_restart(s_reconnect_timer, s_backoff_ms);
        s_backoff_ms = MIN(s_backoff_ms * 2, RECONNECT_MAX_MS);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        portENTER_CRITICAL(&s_lock);
        s_status.connected = true;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        portEXIT_CRITICAL(&s_lock);
        s_backoff_ms = RECONNECT_MIN_MS;
        esp_timer_stop(s_ap_start_timer);
        if (s_status.ap_active) {
            timer_restart(s_ap_stop_timer, AP_STOP_DELAY_MS);
        }
        ESP_LOGI(TAG, "Connected to \"%s\", IP %s, http://%s.local/", s_status.ssid, s_status.ip,
                 s_status.hostname);
        if (ota_server_start() == ESP_OK) {
            ota_confirm_running_app("OTA server reachable");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        portENTER_CRITICAL(&s_lock);
        s_status.ap_clients++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Setup AP: client connected (%d)", s_status.ap_clients);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        portENTER_CRITICAL(&s_lock);
        s_status.ap_clients = MAX(0, s_status.ap_clients - 1);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Setup AP: client left (%d)", s_status.ap_clients);
    }
#if BOARD_HAS_ETH
    else if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
        portENTER_CRITICAL(&s_lock);
        s_status.eth_link = true;
        s_status.eth_speed_mbps = speed == ETH_SPEED_10M ? 10 : speed == ETH_SPEED_100M ? 100 : 1000;
        s_status.eth_full_duplex = duplex == ETH_DUPLEX_FULL;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Ethernet link up, %d Mbps %s duplex", s_status.eth_speed_mbps,
                 s_status.eth_full_duplex ? "full" : "half");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        portENTER_CRITICAL(&s_lock);
        s_status.eth_link = false;
        s_status.eth_connected = false;
        s_status.eth_ip[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "Ethernet link down");
        if (!any_connected()) {
            esp_timer_stop(s_ap_stop_timer);
            ap_arm_start_timer();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        portENTER_CRITICAL(&s_lock);
        s_status.eth_connected = true;
        snprintf(s_status.eth_ip, sizeof(s_status.eth_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        portEXIT_CRITICAL(&s_lock);
        esp_timer_stop(s_ap_start_timer);
        if (s_status.ap_active) {
            timer_restart(s_ap_stop_timer, AP_STOP_DELAY_MS);
        }
        ESP_LOGI(TAG, "Ethernet connected, IP %s, http://%s.local/", s_status.eth_ip, s_status.hostname);
        if (ota_server_start() == ESP_OK) {
            ota_confirm_running_app("OTA server reachable");
        }
    }
#endif
}

#if BOARD_HAS_ETH
/** Ethernet driver (board) + netif with DHCP. Not fatal: Wi-Fi works without it. */
static void eth_start(void)
{
    esp_err_t err = board_eth_init(&s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet unavailable: %s", esp_err_to_name(err));
        return;
    }
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = ETH_ROUTE_PRIO;
    const esp_netif_config_t cfg = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    s_eth_netif = esp_netif_new(&cfg);
    esp_netif_set_hostname(s_eth_netif, s_status.hostname);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, event_handler, NULL));
    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return;
    }
    s_status.eth_present = true;
}
#endif

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(s_status.hostname);
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
    load_hostname();
    esp_netif_set_hostname(s_netif, s_status.hostname);
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ap_netif_setup();

    const esp_timer_create_args_t reconnect_args = {.callback = reconnect_cb, .name = "wifi_reconn"};
    const esp_timer_create_args_t ap_start_args = {.callback = ap_start_cb, .name = "ap_start"};
    const esp_timer_create_args_t ap_stop_args = {.callback = ap_stop_cb, .name = "ap_stop"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_args, &s_reconnect_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&ap_start_args, &s_ap_start_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&ap_stop_args, &s_ap_stop_timer), TAG, "timer");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    esp_log_level_set("wifi", ESP_LOG_WARN); /* we log connect/disconnect ourselves */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage"); /* credentials live in our own namespace */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
#if BOARD_HAS_ETH
    eth_start(); /* before the station starts: without Wi-Fi credentials, the setup AP waits for it */
#endif

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "CDPlayer-%02X%02X", mac[4], mac[5]);

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
        s_force_attempt = true;
        timer_restart(s_reconnect_timer, 500);
        ap_arm_start_timer();
    } else {
        ESP_LOGI(TAG, "Wi-Fi credentials cleared");
        ap_start();
    }
    return ESP_OK;
}

void net_get_status(net_status_t *st)
{
    portENTER_CRITICAL(&s_lock);
    *st = s_status;
    portEXIT_CRITICAL(&s_lock);
}

bool net_is_connected(void)
{
    return any_connected();
}

int net_scan(net_scan_entry_t *out, int max)
{
    const wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 80, .max = 120},
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = heap_caps_calloc(MAX(n, 1), sizeof(*recs), MALLOC_CAP_SPIRAM);
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, recs); /* sorted by RSSI, strongest first */
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < count && !dup; j++) {
            dup = strcmp(out[j].ssid, (const char *)recs[i].ssid) == 0;
        }
        if (!dup) {
            strlcpy(out[count].ssid, (const char *)recs[i].ssid, sizeof(out[count].ssid));
            out[count].rssi = recs[i].rssi;
            out[count].open = recs[i].authmode == WIFI_AUTH_OPEN;
            count++;
        }
    }
    free(recs);
    return count;
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
               st.ssid, st.ip, rssi, st.hostname);
    } else {
        printf("WIFI connecting ssid=\"%s\"\n", st.ssid);
    }
    if (st.eth_present) {
        if (st.eth_connected) {
            printf("ETH connected ip=%s %d Mbps %s duplex url=http://%s.local/\n", st.eth_ip, st.eth_speed_mbps,
                   st.eth_full_duplex ? "full" : "half", st.hostname);
        } else {
            printf("ETH %s\n", st.eth_link ? "link up, waiting for an address" : "no link");
        }
    }
    if (st.ap_active) {
        printf("SETUP_AP ssid=\"%s\" password=\"%s\" clients=%d url=http://" NET_AP_IP_STR "/\n", st.ap_ssid,
               net_ap_password(), st.ap_clients);
    }
    ota_print_info();
    storage_print_info();
    printf("HEAP internal free=%u (min %u) psram free=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
