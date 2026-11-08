/*
 * Captive-portal DNS: every A query is answered with the access point's address; other query types get
 * an empty NOERROR answer. Bound to the AP address only, so the station side is not affected.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "captive_dns.h"

static const char *TAG = "dns";

#define DNS_PORT      53
#define DNS_MAX_LEN   512
#define DNS_HDR_LEN   12
#define TYPE_A        1
#define CLASS_IN      1

static uint32_t s_ip;
static TaskHandle_t s_task;

/** Build the answer in place. @return response length, 0 to drop the packet */
static int make_answer(uint8_t *pkt, int len)
{
    if (len < DNS_HDR_LEN || (pkt[2] & 0x80) || (pkt[2] & 0x78) != 0) {
        return 0; /* not a standard query */
    }
    int qdcount = pkt[4] << 8 | pkt[5];
    if (qdcount != 1) {
        return 0;
    }
    /* Walk the question name */
    int p = DNS_HDR_LEN;
    while (p < len && pkt[p] != 0) {
        if ((pkt[p] & 0xC0) != 0) {
            return 0; /* no compression in questions */
        }
        p += pkt[p] + 1;
    }
    if (p + 5 > len) {
        return 0;
    }
    int qtype = pkt[p + 1] << 8 | pkt[p + 2];
    int qclass = pkt[p + 3] << 8 | pkt[p + 4];
    int end = p + 5; /* end of the question; drop anything after it (EDNS etc.) */

    pkt[2] = 0x84 | (pkt[2] & 0x01); /* response, authoritative, keep RD */
    pkt[3] = 0x00;                   /* RA=0, NOERROR */
    pkt[6] = pkt[7] = 0;             /* ANCOUNT */
    pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;
    if (qtype != TYPE_A || qclass != CLASS_IN || end + 16 > DNS_MAX_LEN) {
        return end; /* no answer for AAAA etc. */
    }
    pkt[7] = 1;
    uint8_t *a = pkt + end;
    a[0] = 0xC0;  /* name: pointer to the question */
    a[1] = DNS_HDR_LEN;
    a[2] = 0;
    a[3] = TYPE_A;
    a[4] = 0;
    a[5] = CLASS_IN;
    a[6] = 0;     /* TTL 60 s */
    a[7] = 0;
    a[8] = 0;
    a[9] = 60;
    a[10] = 0;    /* RDLENGTH 4 */
    a[11] = 4;
    memcpy(a + 12, &s_ip, 4);
    return end + 16;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = s_ip,
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Cannot bind UDP port %d (errno %d)", DNS_PORT, errno);
        if (sock >= 0) {
            close(sock);
        }
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Captive DNS on " IPSTR ":%d", IP2STR((esp_ip4_addr_t *)&s_ip), DNS_PORT);
    static uint8_t pkt[DNS_MAX_LEN];
    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        int out = make_answer(pkt, len);
        if (out > 0) {
            sendto(sock, pkt, out, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

esp_err_t captive_dns_start(uint32_t ip)
{
    if (s_task) {
        return ESP_OK;
    }
    s_ip = ip;
    return xTaskCreate(dns_task, "dns", 3072, NULL, 3, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
