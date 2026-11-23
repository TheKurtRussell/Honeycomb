/*
 * Honeycomb — BLE (NimBLE observer) over the C6.
 *
 * The NimBLE host runs on the P4; the BT controller is on the C6, reached over
 * ESP-Hosted HCI (SDIO). hc_ble_start() brings the host up; hc_ble_scan() runs
 * an observer scan whose advertisement reports are collected, de-duplicated by
 * address, into a table the UI can read with hc_ble_get().
 */
#ifndef HC_BLE_H
#define HC_BLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr[6];
    int8_t  rssi;
    char    name[32];   /* "" if the advertiser sent no name */
} hc_ble_dev_t;

/* Bring up the NimBLE host bound to the C6 controller. Returns true if the
 * stack started (the controller syncs asynchronously; see hc_ble_ready). */
bool hc_ble_start(void);

/* True once the host has synced with the C6 controller (ready to scan). */
bool hc_ble_ready(void);

/* Start (or restart) an observer scan for `ms` milliseconds. Clears the table
 * first. No-op if not ready or already scanning. */
void hc_ble_scan(uint32_t ms);

/* True while a scan is in progress. */
bool hc_ble_scanning(void);

/* Snapshot up to `max` discovered devices into `out`, sorted strongest-first.
 * Returns the number written. Safe to call from any task. */
int hc_ble_get(hc_ble_dev_t *out, int max);

#ifdef __cplusplus
}
#endif
#endif /* HC_BLE_H */
