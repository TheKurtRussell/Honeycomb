/*
 * Honeycomb — surveillance-infrastructure signatures
 *
 * Passive classification of fixed surveillance gear (Flock Safety ALPR cameras
 * to start) from the WiFi scan data we already collect. Receive/detect only —
 * this maps what is watching public streets, it does not touch the devices.
 *
 * Confidence tiers:
 *   SSID match  = HIGH   (the unit's own management/battery SSID)
 *   OUI match   = LOW    (Flock uses third-party radio chips, so the MAC OUI
 *                         belongs to the chip vendor and other gear can share it)
 * The reliable method (wildcard probe-request + 802.11 IE matching) needs
 * promiscuous-mode capture — a custom C6 slave — and will land in a later tier.
 */
#ifndef HC_SIGS_H
#define HC_SIGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HC_SIG_NONE = 0,
    HC_SIG_FLOCK_OUI,    // low confidence  — chip-vendor OUI match
    HC_SIG_FLOCK_SSID,   // high confidence — Flock management/battery SSID
};

/* Classify a WiFi AP by SSID + BSSID. Returns an HC_SIG_* value. */
int         hc_sig_wifi(const char *ssid, const uint8_t mac[6]);
/* Human-readable label for a classification ("" for NONE). */
const char *hc_sig_label(int sig);
/* 2 = high, 1 = low, 0 = none. */
int         hc_sig_confidence(int sig);

#ifdef __cplusplus
}
#endif
#endif /* HC_SIGS_H */
