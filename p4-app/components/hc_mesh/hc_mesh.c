/* Honeycomb — LAN mesh discovery (see hc_mesh.h) */
#include "hc_mesh.h"
#include "hc_scanlog.h"

#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#define TAG        "hc_mesh"
#define MESH_PORT  54621
#define MESH_MAGIC "HNYC"
#define PEER_MAX   16
#define PEER_STALE 20          /* drop peers unheard for this many seconds */

static hc_mesh_peer_t    s_peer[PEER_MAX];
static int               s_npeer;
static SemaphoreHandle_t s_lock;
static volatile bool     s_run;
static TaskHandle_t      s_task;
static char              s_name[24];
static char              s_ip[16];

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static void make_name(void)
{
    if (s_name[0]) return;
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_name, sizeof(s_name), "hc-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

/* Current STA IP as a string; empty if not up. Returns true if we have one. */
static bool self_ip(char *out, size_t cap)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) {
        if (out && cap) out[0] = 0;
        return false;
    }
    if (out) snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
    return true;
}

/* Merge a received beacon into the peer table (caller does not hold lock). */
static void peer_update(const char *ip, const char *name,
                        uint32_t up, uint32_t wc, uint32_t bc)
{
    if (!ip || !name) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i;
    for (i = 0; i < s_npeer; i++)
        if (strcmp(s_peer[i].ip, ip) == 0) break;
    if (i == s_npeer && s_npeer < PEER_MAX) s_npeer++;
    if (i < PEER_MAX) {
        strlcpy(s_peer[i].ip, ip, sizeof(s_peer[i].ip));
        strlcpy(s_peer[i].name, name, sizeof(s_peer[i].name));
        s_peer[i].uptime_s = up;
        s_peer[i].wifi_cnt = wc;
        s_peer[i].ble_cnt  = bc;
        s_peer[i].age_s    = (int32_t)now_s();   /* stash last-heard; converted on read */
    }
    xSemaphoreGive(s_lock);
}

static void mesh_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket failed"); s_run = false; s_task = NULL; vTaskDelete(NULL); return; }

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    struct sockaddr_in bind_a = {0};
    bind_a.sin_family = AF_INET;
    bind_a.sin_port = htons(MESH_PORT);
    bind_a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_a, sizeof(bind_a)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(sock); s_run = false; s_task = NULL; vTaskDelete(NULL); return;
    }

    /* 500ms recv timeout so the loop can also send beacons and honour stop. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bcast = {0};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(MESH_PORT);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);   /* 255.255.255.255 */

    uint32_t last_tx = 0;
    while (s_run) {
        /* Beacon every ~2s. */
        uint32_t t = now_s();
        if (t - last_tx >= 2) {
            last_tx = t;
            self_ip(s_ip, sizeof(s_ip));
            char pkt[96];
            int len = snprintf(pkt, sizeof(pkt), "%s|%s|%lu|%d|%d",
                               MESH_MAGIC, s_name, (unsigned long)now_s(),
                               hc_scanlog_wifi_count(), hc_scanlog_ble_count());
            sendto(sock, pkt, len, 0, (struct sockaddr *)&bcast, sizeof(bcast));
        }

        /* Receive any beacons. */
        char buf[128];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int r = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (r > 0) {
            buf[r] = 0;
            char fromip[16];
            inet_ntoa_r(from.sin_addr, fromip, sizeof(fromip));
            /* Ignore our own broadcast echo. */
            if (strcmp(fromip, s_ip) != 0 &&
                strncmp(buf, MESH_MAGIC "|", 5) == 0) {
                char name[24] = ""; unsigned long up = 0; int wc = 0, bc = 0;
                /* HNYC|name|uptime|wifi|ble */
                if (sscanf(buf + 5, "%23[^|]|%lu|%d|%d", name, &up, &wc, &bc) >= 1)
                    peer_update(fromip, name, (uint32_t)up, (uint32_t)wc, (uint32_t)bc);
            }
        }

        /* Age out stale peers. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t nw = now_s();
        for (int i = 0; i < s_npeer; ) {
            if ((int32_t)(nw - (uint32_t)s_peer[i].age_s) > PEER_STALE) {
                s_peer[i] = s_peer[s_npeer - 1];
                s_npeer--;
            } else i++;
        }
        xSemaphoreGive(s_lock);
    }

    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

bool hc_mesh_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    make_name();
    if (!self_ip(s_ip, sizeof(s_ip))) return false;   /* no WiFi IP yet */
    if (s_run) return true;
    s_run = true;
    s_npeer = 0;
    if (xTaskCreate(mesh_task, "hc_mesh", 4096, NULL, 4, &s_task) != pdPASS) {
        s_run = false;
        return false;
    }
    return true;
}

void hc_mesh_stop(void)
{
    s_run = false;   /* task notices within its 500ms recv timeout and exits */
}

bool hc_mesh_running(void)  { return s_run; }
const char *hc_mesh_self_name(void) { make_name(); return s_name; }
const char *hc_mesh_self_ip(void)   { return s_ip; }

int hc_mesh_get(hc_mesh_peer_t *out, int max)
{
    if (!out || max <= 0 || !s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_npeer < max ? s_npeer : max;
    uint32_t nw = now_s();
    for (int i = 0; i < n; i++) {
        out[i] = s_peer[i];
        out[i].age_s = (int32_t)(nw - (uint32_t)s_peer[i].age_s);  /* now = seconds ago */
    }
    xSemaphoreGive(s_lock);
    return n;
}
