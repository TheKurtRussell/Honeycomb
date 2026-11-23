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

/* Item-tracker classification (counter-surveillance). */
enum {
    HC_TRACK_NONE = 0,
    HC_TRACK_APPLE,    /* Apple Find My / AirTag (separated) */
    HC_TRACK_TILE,     /* Tile */
    HC_TRACK_SAMSUNG,  /* Samsung SmartTag / Galaxy Find */
    HC_TRACK_GENERIC,  /* other known finder service */
};

typedef struct {
    uint8_t  addr[6];
    int8_t   rssi;
    uint8_t  addr_type;   /* 0 = public (OUI-identifiable), else random/private */
    uint16_t company_id;  /* BLE SIG company ID from mfg data, 0xFFFF if none */
    uint16_t appearance;  /* GAP appearance, 0 if not advertised */
    uint16_t svc_uuid;    /* first 16-bit (service/service-data) UUID, 0 if none */
    uint8_t  tracker;     /* HC_TRACK_* classification */
    bool     connectable; /* advertiser accepts connections */
    char     name[32];    /* "" if the advertiser sent no name */
} hc_ble_dev_t;

/* Human-readable vendor for a BLE SIG company ID ("" if unknown). */
const char *hc_ble_vendor(uint16_t company_id);
/* Human-readable device kind for a GAP appearance value ("" if unknown/0). */
const char *hc_ble_appearance_name(uint16_t appearance);
/* Human-readable item-tracker label for a HC_TRACK_* value ("" if NONE). */
const char *hc_ble_tracker_name(uint8_t tracker);

/* Total advertisement reports received since start (for a rate/traffic meter). */
uint32_t hc_ble_adv_total(void);

/* ---- counter-surveillance: ring a suspected item tracker ----
 *
 * Connects to the device as a BLE central and issues the DULT standard
 * "non-owner play sound" command (works on modern AirTags / Find My network
 * trackers while they are separated from their owner). Asynchronous: call
 * hc_ble_ring(), then poll hc_ble_ring_state(). Only one ring at a time. */
enum {
    HC_RING_IDLE = 0,
    HC_RING_CONNECTING,
    HC_RING_DISCOVERING,
    HC_RING_WRITING,
    HC_RING_OK,        /* command accepted — the tag should be chirping */
    HC_RING_FAIL,      /* couldn't connect / no sound service / rejected */
};

/* Begin ringing. Returns false if BLE isn't ready or a ring is already in
 * flight. Pauses scanning for the duration. */
bool    hc_ble_ring(const uint8_t addr[6], uint8_t addr_type);
uint8_t hc_ble_ring_state(void);
bool    hc_ble_ring_busy(void);
/* Diagnostic detail for the last/active ring attempt (stage + error code). */
const char *hc_ble_ring_msg(void);

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

/* Stop an in-progress scan (no-op if idle). */
void hc_ble_stop(void);

/* Snapshot up to `max` discovered devices into `out`, sorted strongest-first.
 * Returns the number written. Safe to call from any task. */
int hc_ble_get(hc_ble_dev_t *out, int max);

/* Current RSSI for a specific address, or -128 if not in the table. For foxhunt. */
int hc_ble_find(const uint8_t *addr);

#ifdef __cplusplus
}
#endif
#endif /* HC_BLE_H */
