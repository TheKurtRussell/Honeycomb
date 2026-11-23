/* Honeycomb — scan logbook (see hc_scanlog.h) */
#include "hc_scanlog.h"
#include "hc_storage.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "hc_scanlog"

#define WIFI_MAX 128
#define BLE_MAX  128

#define WIFI_CSV "/sdcard/wifi_log.csv"
#define BLE_CSV  "/sdcard/ble_log.csv"

static hc_scanlog_entry_t s_wifi[WIFI_MAX];
static int                s_wifi_n;
static hc_scanlog_entry_t s_ble[BLE_MAX];
static int                s_ble_n;
static SemaphoreHandle_t  s_lock;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

void hc_scanlog_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    /* Drop a header on each file if it is empty / absent (append is harmless if
     * the file already has rows — we only add the header when nothing read). */
    char probe[4]; size_t len = 0;
    if (hc_storage_read_head(WIFI_CSV, probe, sizeof(probe), &len) != 0 || len == 0)
        hc_storage_append(WIFI_CSV, "uptime_s,mac,rssi,channel,ssid\n");
    len = 0;
    if (hc_storage_read_head(BLE_CSV, probe, sizeof(probe), &len) != 0 || len == 0)
        hc_storage_append(BLE_CSV, "uptime_s,mac,rssi,company,name\n");
}

/* Find mac in table; return index or -1. Caller holds the lock. */
static int find(const hc_scanlog_entry_t *tbl, int n, const uint8_t mac[6])
{
    for (int i = 0; i < n; i++)
        if (memcmp(tbl[i].mac, mac, 6) == 0) return i;
    return -1;
}

bool hc_scanlog_wifi(const char *ssid, const uint8_t mac[6], int rssi, int channel)
{
    if (!mac) return false;
    if (!s_lock) hc_scanlog_init();
    bool is_new = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find(s_wifi, s_wifi_n, mac);
    if (i < 0 && s_wifi_n < WIFI_MAX) {
        i = s_wifi_n++;
        memset(&s_wifi[i], 0, sizeof(s_wifi[i]));
        memcpy(s_wifi[i].mac, mac, 6);
        s_wifi[i].rssi_best = (int8_t)rssi;
        s_wifi[i].first_s = now_s();
        is_new = true;
    }
    if (i >= 0) {
        s_wifi[i].rssi_last = (int8_t)rssi;
        if (rssi > s_wifi[i].rssi_best) s_wifi[i].rssi_best = (int8_t)rssi;
        s_wifi[i].channel = (uint8_t)channel;
        s_wifi[i].company = 0xFFFF;
        s_wifi[i].count++;
        s_wifi[i].last_s = now_s();
        if (ssid && ssid[0]) strlcpy(s_wifi[i].label, ssid, sizeof(s_wifi[i].label));
    }
    xSemaphoreGive(s_lock);

    if (is_new) {
        char line[160];
        snprintf(line, sizeof(line),
                 "%lu,%02X:%02X:%02X:%02X:%02X:%02X,%d,%d,%s\n",
                 (unsigned long)now_s(),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 rssi, channel, (ssid && ssid[0]) ? ssid : "");
        hc_storage_append(WIFI_CSV, line);
    }
    return is_new;
}

bool hc_scanlog_ble(const uint8_t mac[6], int rssi, uint16_t company, const char *name)
{
    if (!mac) return false;
    if (!s_lock) hc_scanlog_init();
    bool is_new = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find(s_ble, s_ble_n, mac);
    if (i < 0 && s_ble_n < BLE_MAX) {
        i = s_ble_n++;
        memset(&s_ble[i], 0, sizeof(s_ble[i]));
        memcpy(s_ble[i].mac, mac, 6);
        s_ble[i].rssi_best = (int8_t)rssi;
        s_ble[i].first_s = now_s();
        is_new = true;
    }
    if (i >= 0) {
        s_ble[i].rssi_last = (int8_t)rssi;
        if (rssi > s_ble[i].rssi_best) s_ble[i].rssi_best = (int8_t)rssi;
        s_ble[i].channel = 0;
        s_ble[i].company = company;
        s_ble[i].count++;
        s_ble[i].last_s = now_s();
        if (name && name[0]) strlcpy(s_ble[i].label, name, sizeof(s_ble[i].label));
    }
    xSemaphoreGive(s_lock);

    if (is_new) {
        char line[160];
        snprintf(line, sizeof(line),
                 "%lu,%02X:%02X:%02X:%02X:%02X:%02X,%d,0x%04X,%s\n",
                 (unsigned long)now_s(),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 rssi, company, (name && name[0]) ? name : "");
        hc_storage_append(BLE_CSV, line);
    }
    return is_new;
}

/* Copy entries most-recently-seen first. Caller-supplied buffer. */
static int snapshot(const hc_scanlog_entry_t *tbl, int n, hc_scanlog_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int cnt = n < max ? n : max;
    /* selection by last_s, newest first (n is small, simple pass is fine) */
    bool used[WIFI_MAX > BLE_MAX ? WIFI_MAX : BLE_MAX] = {0};
    for (int o = 0; o < cnt; o++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (used[i]) continue;
            if (best < 0 || tbl[i].last_s > tbl[best].last_s) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        out[o] = tbl[best];
    }
    xSemaphoreGive(s_lock);
    return cnt;
}

int hc_scanlog_get_wifi(hc_scanlog_entry_t *out, int max) { return snapshot(s_wifi, s_wifi_n, out, max); }
int hc_scanlog_get_ble(hc_scanlog_entry_t *out, int max)  { return snapshot(s_ble,  s_ble_n,  out, max); }

int hc_scanlog_wifi_count(void) { return s_wifi_n; }
int hc_scanlog_ble_count(void)  { return s_ble_n; }

const char *hc_scanlog_wifi_path(void) { return WIFI_CSV; }
const char *hc_scanlog_ble_path(void)  { return BLE_CSV; }
