/*
 * Honeycomb — P4 storage: microSD mount + pcap writer
 *
 * Writes captured 802.15.4 frames to a standard pcap file that opens directly
 * in Wireshark. Link type is LINKTYPE_IEEE802_15_4_WITHFCS (195): the C6
 * forwards the full MPDU including the 2-byte FCS, so Wireshark can show FCS
 * validity.
 */
#ifndef HC_STORAGE_H
#define HC_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hc_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the Tab5 microSD (SDMMC). Safe to call once at boot. */
esp_err_t hc_storage_mount(void);
bool      hc_storage_is_mounted(void);

/* List up to `max` *.bin files on the SD (root and /honeycomb), newest-dir
 * last. Fills full paths into paths[i] (each HC_PATH_MAX bytes). Count in *n. */
#define HC_PATH_MAX 128
esp_err_t hc_storage_list_bins(char (*paths)[HC_PATH_MAX], int max, int *n);

/* Save a text blob (e.g. the serial monitor log) to a fresh
 * /sdcard/honeycomb/serial-NNN.txt and return its path in path_out. */
esp_err_t hc_storage_save_log(const char *text, char *path_out, size_t cap);

/* pcap lifecycle. Open picks a fresh /sdcard/honeycomb/capNNN.pcap name and
 * returns it in path_out. */
esp_err_t hc_storage_pcap_open(char *path_out, size_t path_cap);
esp_err_t hc_storage_pcap_write(const hc_frame_hdr_t *meta, const uint8_t *mpdu);
esp_err_t hc_storage_pcap_close(void);

#ifdef __cplusplus
}
#endif
#endif /* HC_STORAGE_H */
