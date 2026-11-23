/*
 * Honeycomb — host-driven C6 slave OTA (over the ESP-Hosted SDIO link)
 *
 * Streams a staged ESP-Hosted coprocessor image (from a P4 flash partition) to
 * the on-board ESP32-C6 using esp_hosted_slave_ota_*, then (separately)
 * activates it and restarts the P4 to resync with the freshly-booted slave.
 *
 * No downloader kit, no soldering: the image rides the same SDIO link the C6
 * already uses. Safety: the slave validates the fully-written image (SHA256) at
 * ota_end BEFORE we activate, so a corrupted transfer leaves the C6 booting the
 * existing (factory) slave unchanged — we only switch it over once it verifies.
 */
#ifndef HC_C6OTA_H
#define HC_C6OTA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Progress callback: pct 0..100, msg is a short status line. Called from the
 * OTA worker (not an ISR); keep it light and don't block. */
typedef void (*hc_c6ota_cb)(int pct, const char *msg, void *ctx);

/* Stream the slave image in data partition `label` to the C6 (begin/write/end).
 * Validates the image header first. Returns ESP_OK on success; does NOT
 * activate or restart. */
esp_err_t hc_c6ota_from_partition(const char *label, hc_c6ota_cb cb, void *ctx);

/* Activate the just-written image and restart the P4 to resync with the slave.
 * Does not return (calls esp_restart). Requires slave fw >= 2.6.0. */
void hc_c6ota_activate_and_restart(void);

#ifdef __cplusplus
}
#endif
#endif /* HC_C6OTA_H */
