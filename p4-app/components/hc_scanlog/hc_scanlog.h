/*
 * Honeycomb — scan logbook
 *
 * Keeps a de-duplicated, persistent record of every WiFi AP and BLE device
 * seen, so contacts can be tracked and identified over time instead of
 * scrolling past in a live scan. Each unique MAC is kept in a RAM table
 * (first/last seen, times seen, best RSSI) and appended once to a CSV on the
 * SD card for export.
 */
#ifndef HC_SCANLOG_H
#define HC_SCANLOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  mac[6];
    char     label[33];   /* SSID (wifi) or name/vendor (ble) */
    int8_t   rssi_best;   /* strongest RSSI seen */
    int8_t   rssi_last;   /* most recent RSSI */
    uint8_t  channel;     /* wifi primary channel (0 for ble) */
    uint16_t company;     /* ble company id (0xFFFF if none) */
    uint32_t count;       /* number of sightings */
    uint32_t first_s;     /* uptime seconds, first seen */
    uint32_t last_s;      /* uptime seconds, last seen */
} hc_scanlog_entry_t;

/* Set up the mutex and, if the SD is mounted, ensure the CSV files have a
 * header row. Safe to call once at boot (SD mount can come before or after). */
void hc_scanlog_init(void);

/* Record a sighting. De-dups by MAC in RAM; the first time a MAC is seen it is
 * also appended to the SD CSV. Returns true on a first (new) sighting. */
bool hc_scanlog_wifi(const char *ssid, const uint8_t mac[6], int rssi, int channel);
bool hc_scanlog_ble(const uint8_t mac[6], int rssi, uint16_t company, const char *name);

/* Copy up to `max` entries into out, most-recently-seen first. Returns count. */
int  hc_scanlog_get_wifi(hc_scanlog_entry_t *out, int max);
int  hc_scanlog_get_ble(hc_scanlog_entry_t *out, int max);

int  hc_scanlog_wifi_count(void);
int  hc_scanlog_ble_count(void);

/* SD CSV paths (for the file viewer / export). */
const char *hc_scanlog_wifi_path(void);
const char *hc_scanlog_ble_path(void);

#ifdef __cplusplus
}
#endif
#endif /* HC_SCANLOG_H */
