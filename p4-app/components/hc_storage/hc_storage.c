/*
 * Honeycomb — P4 storage implementation (SDMMC + pcap)
 *
 * Tab5 microSD is on the P4's SDMMC slot (see docs/hardware.md for the pin set
 * reported by the board docs: G39-G44). We use the 1-bit or 4-bit SDMMC host via
 * the IDF sdmmc driver. If your board revision wires SD on SPI instead, switch
 * the host init block (clearly marked).
 */
#include "hc_storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

#define TAG "hc_store"
#define MOUNT_POINT "/sdcard"
#define CAP_DIR     MOUNT_POINT "/honeycomb"

#define LINKTYPE_IEEE802_15_4_WITHFCS 195

static bool  s_mounted;
static FILE *s_pcap;

/* pcap classic global header */
typedef struct __attribute__((packed)) {
    uint32_t magic;        /* 0xa1b2c3d4 */
    uint16_t ver_major, ver_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;      /* link type */
} pcap_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec, ts_usec;
    uint32_t incl_len, orig_len;
} pcap_rec_t;

esp_err_t hc_storage_mount(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;

    /* Tab5 microSD on the P4's SDMMC. On ESP32-P4 the SD card is on SDMMC
     * SLOT 0; the on-board ESP32-C6 uses SLOT 1 of the same controller for
     * ESP-Hosted. Pinning the SD to slot 0 explicitly (with the Tab5's
     * GPIO-matrix pins CLK=43 CMD=44 D0-3=39-42) keeps it clear of the C6 so
     * both can share the one SDMMC controller once Wi-Fi is enabled. This is
     * the config from Espressif's mcu_hosted_sdio_sdmmc_combined example. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;              /* 1-bit for reliability during bring-up */
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0  = GPIO_NUM_39;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t e = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(e)); return e; }

    mkdir(CAP_DIR, 0775);
    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted (slot 0, 1-bit, CLK43/CMD44/D0=39)");
    return ESP_OK;
}
bool hc_storage_is_mounted(void) { return s_mounted; }

static bool ends_with_bin(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    const char *e = name + n - 4;
    return (e[0] == '.') &&
           (e[1] == 'b' || e[1] == 'B') &&
           (e[2] == 'i' || e[2] == 'I') &&
           (e[3] == 'n' || e[3] == 'N');
}

static void scan_dir_for_bins(const char *dir, char (*paths)[HC_PATH_MAX],
                              int max, int *n)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while (*n < max && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;        /* skip ., .., hidden */
        if (!ends_with_bin(de->d_name)) continue;
        snprintf(paths[*n], HC_PATH_MAX, "%s/%s", dir, de->d_name);
        (*n)++;
    }
    closedir(d);
}

esp_err_t hc_storage_list_bins(char (*paths)[HC_PATH_MAX], int max, int *n)
{
    if (!paths || !n || max <= 0) return ESP_ERR_INVALID_ARG;
    *n = 0;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    scan_dir_for_bins(MOUNT_POINT, paths, max, n);
    scan_dir_for_bins(CAP_DIR, paths, max, n);
    return ESP_OK;
}

esp_err_t hc_storage_list_dir(const char *dir, hc_dirent_t *out, int max, int *n)
{
    if (!dir || !out || !n || max <= 0) return ESP_ERR_INVALID_ARG;
    *n = 0;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    DIR *d = opendir(dir);
    if (!d) return ESP_FAIL;
    struct dirent *de;
    while (*n < max && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;   /* skip ., .., hidden */
        hc_dirent_t *e = &out[*n];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        char full[HC_PATH_MAX + 66];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size   = (uint32_t)st.st_size;
        } else {
            e->is_dir = (de->d_type == DT_DIR);
            e->size   = 0;
        }
        (*n)++;
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t hc_storage_save_log(const char *text, char *path_out, size_t cap)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    mkdir(CAP_DIR, 0777);                 /* ok if it already exists */
    for (int i = 0; i < 1000; i++) {
        char p[HC_PATH_MAX];
        snprintf(p, sizeof(p), "%s/serial-%03d.txt", CAP_DIR, i);
        struct stat st;
        if (stat(p, &st) == 0) continue;  /* taken — try next */
        FILE *f = fopen(p, "w");
        if (!f) return ESP_FAIL;
        if (text && *text) fwrite(text, 1, strlen(text), f);
        fclose(f);
        if (path_out) snprintf(path_out, cap, "%s", p);
        ESP_LOGI(TAG, "saved serial log to %s", p);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t hc_storage_pcap_open(char *path_out, size_t cap)
{
    if (!s_mounted) { esp_err_t e = hc_storage_mount(); if (e != ESP_OK) return e; }
    if (s_pcap) return ESP_ERR_INVALID_STATE;

    /* find a free capNNN.pcap */
    char path[96];
    for (int n = 0; n < 1000; ++n) {
        snprintf(path, sizeof(path), CAP_DIR "/cap%03d.pcap", n);
        struct stat st;
        if (stat(path, &st) != 0) break;
    }
    s_pcap = fopen(path, "wb");
    if (!s_pcap) return ESP_FAIL;

    pcap_hdr_t gh = {
        .magic = 0xa1b2c3d4, .ver_major = 2, .ver_minor = 4,
        .thiszone = 0, .sigfigs = 0, .snaplen = 256,
        .network = LINKTYPE_IEEE802_15_4_WITHFCS,
    };
    fwrite(&gh, sizeof(gh), 1, s_pcap);
    fflush(s_pcap);
    if (path_out) strncpy(path_out, path, cap);
    return ESP_OK;
}

esp_err_t hc_storage_pcap_write(const hc_frame_hdr_t *meta, const uint8_t *mpdu)
{
    if (!s_pcap) return ESP_ERR_INVALID_STATE;
    pcap_rec_t rh = {
        .ts_sec  = (uint32_t)(meta->ts_us / 1000000ull),
        .ts_usec = (uint32_t)(meta->ts_us % 1000000ull),
        .incl_len = meta->len, .orig_len = meta->len,
    };
    fwrite(&rh, sizeof(rh), 1, s_pcap);
    fwrite(mpdu, 1, meta->len, s_pcap);
    /* periodic flush so a yanked card still yields a usable file */
    static uint16_t n; if ((++n & 0x1F) == 0) fflush(s_pcap);
    return ESP_OK;
}

esp_err_t hc_storage_pcap_close(void)
{
    if (!s_pcap) return ESP_OK;
    fflush(s_pcap); fclose(s_pcap); s_pcap = NULL;
    return ESP_OK;
}
