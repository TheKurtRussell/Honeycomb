/*
 * Honeycomb — IEEE 802.15.4 (Zigbee/Thread/Matter) scan, host side.
 *
 * Triggers a time-sliced 802.15.4 activity scan on the C6 and collects the
 * per-channel results, over the ESP-Hosted Peer Data (Custom RPC) channel.
 * Receive/detect only — this maps activity on the Zigbee/Thread band, it does
 * not join or transmit onto any network.
 */
#ifndef HC_Z154_H
#define HC_Z154_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t channel;   /* 11..26 */
    uint8_t frames;    /* frames seen during the dwell (capped 255) */
    int8_t  rssi;      /* strongest RSSI seen (-128 = none) */
    uint8_t lqi;       /* link quality of the last frame */
} hc_z154_chan_t;

/* Register the result handler. Call once after esp_hosted is up. */
void     hc_z154_init(void);

/* Ask the C6 to scan. dwell_10ms = per-channel dwell in 10ms units (0 = default
 * ~120ms). Non-blocking (the request is sent from a worker). Returns false only
 * if a scan is already in flight. */
bool     hc_z154_scan(uint8_t dwell_10ms);

bool     hc_z154_scanning(void);       /* request sent, result not yet back */
bool     hc_z154_have_result(void);    /* at least one result received */
uint8_t  hc_z154_status(void);         /* last result: 0 ok, 1 radio unavailable, 0xFF none */
uint32_t hc_z154_last_ms(void);        /* uptime ms of the last result */

/* Copy the per-channel results (up to max). Returns count. */
int      hc_z154_get(hc_z154_chan_t *out, int max);

#ifdef __cplusplus
}
#endif
#endif /* HC_Z154_H */
