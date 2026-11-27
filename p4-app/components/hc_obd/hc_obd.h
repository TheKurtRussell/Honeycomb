/*
 * Honeycomb — car diagnostics over a BLE ELM327 dongle (read-only)
 *
 * Connects to a Bluetooth ELM327 OBD-II dongle as a BLE central, speaks the
 * ELM327 AT/OBD command set over its serial-style GATT characteristics, and
 * reads Hyundai Ioniq 5 (E-GMP) manufacturer PIDs for battery health.
 *
 * Read-only by design: no bidirectional/actuation commands, no DTC clearing —
 * this observes the car, it does not command it.
 */
#ifndef HC_OBD_H
#define HC_OBD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HC_OBD_IDLE = 0,
    HC_OBD_CONNECTING,
    HC_OBD_DISCOVERING,
    HC_OBD_INIT,        /* running ELM327 handshake */
    HC_OBD_POLLING,     /* handshake done, reading PIDs */
    HC_OBD_ERROR,
};

typedef struct {
    bool     valid;        /* at least one poll cycle completed */
    float    soc_bms;      /* % — true BMS state of charge */
    float    soh;          /* % — state of health */
    float    cell_min_v;   /* V */
    float    cell_max_v;   /* V */
    float    cell_delta_v; /* V — max-min (weak-cell indicator) */
    int      temp_max;     /* C — hottest battery module */
    int      temp_min;     /* C */
    float    dc_voltage;   /* V — pack voltage */
    float    dc_current;   /* A — pack current (+charge/-discharge) */
    float    aux_12v;      /* V — 12V auxiliary battery */
    uint32_t updated_ms;   /* uptime of last successful cycle */
} hc_obd_metrics_t;

/* Register the BLE event handlers. Call once after the BLE host is up. */
void        hc_obd_init(void);

/* Connect to a BLE ELM327 at this address. Pauses BLE scanning for the session.
 * Returns false if BLE isn't ready or already connected/connecting. */
bool        hc_obd_connect(const uint8_t addr[6], uint8_t addr_type);
void        hc_obd_disconnect(void);

uint8_t     hc_obd_state(void);
const char *hc_obd_status(void);          /* short human status line */
void        hc_obd_get(hc_obd_metrics_t *out);
/* Last raw ELM327 response line (for on-device decode iteration). */
const char *hc_obd_last_raw(void);

/* True if a BLE advertiser name looks like an ELM327 OBD dongle. */
bool        hc_obd_name_is_dongle(const char *name);

#ifdef __cplusplus
}
#endif
#endif /* HC_OBD_H */
