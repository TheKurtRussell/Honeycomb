/*
 * Honeycomb — host-driven C6 slave OTA implementation.
 *
 * Image-size parsing and the chunked begin/write/end sequence follow Espressif's
 * esp-hosted "coprocessor_ota" partition example; wrapped here with a progress
 * callback and the Honeycomb activate/restart flow.
 */
#include "hc_c6ota.h"

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "nvs.h"

#define TAG "hc_c6ota"
/* Max OTA write payload is EH_HOST_CP_OTA_CHUNK_MAX (SDIO 1536 buf minus the
 * V2 header, TLV envelope and OTA proto overhead ≈ 1480). Stay safely under it;
 * a chunk over the limit makes esp_hosted_slave_ota_write return ESP_FAIL. */
#define CHUNK 1024

/* Compute the exact image length stored at the start of `part` (header +
 * segments + checksum + optional SHA256), so we send the real image, not the
 * 0xFF-padded remainder of the partition. */
static esp_err_t image_size(const esp_partition_t *part, size_t *out)
{
    esp_image_header_t ih;
    esp_err_t r = esp_partition_read(part, 0, &ih, sizeof(ih));
    if (r != ESP_OK) return r;
    if (ih.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "staged image bad magic 0x%02x", ih.magic);
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = sizeof(ih), total = sizeof(ih);
    for (int i = 0; i < ih.segment_count; i++) {
        esp_image_segment_header_t sh;
        r = esp_partition_read(part, off, &sh, sizeof(sh));
        if (r != ESP_OK) return r;
        total += sizeof(sh) + sh.data_len;
        off   += sizeof(sh) + sh.data_len;
    }
    total += (16 - (total % 16)) % 16;   /* pad to 16 */
    total += 1;                          /* checksum byte */
    if (ih.hash_appended == 1) {
        total += (16 - (total % 16)) % 16;
        total += 32;                     /* SHA256 */
    }
    *out = total;
    return ESP_OK;
}

esp_err_t hc_c6ota_from_partition(const char *label, hc_c6ota_cb cb, void *ctx)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) { ESP_LOGE(TAG, "partition '%s' not found", label); return ESP_ERR_NOT_FOUND; }

    size_t fw_size = 0;
    esp_err_t r = image_size(part, &fw_size);
    if (r != ESP_OK) { if (cb) cb(0, "bad staged image", ctx); return r; }
    if (fw_size == 0 || fw_size > part->size) {
        ESP_LOGE(TAG, "bad image size %u (part %u)", (unsigned)fw_size, (unsigned)part->size);
        if (cb) cb(0, "bad image size", ctx);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "staged C6 image: %u bytes", (unsigned)fw_size);

    if (cb) cb(0, "starting...", ctx);
    r = esp_hosted_slave_ota_begin();
    if (r != ESP_OK) { ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(r)); if (cb) cb(0, "begin failed", ctx); return r; }

    uint8_t buf[CHUNK];
    size_t off = 0;
    while (off < fw_size) {
        size_t n = (fw_size - off > CHUNK) ? CHUNK : (fw_size - off);
        r = esp_partition_read(part, off, buf, n);
        if (r != ESP_OK) { ESP_LOGE(TAG, "read: %s", esp_err_to_name(r)); esp_hosted_slave_ota_end(); if (cb) cb(0, "read failed", ctx); return r; }
        r = esp_hosted_slave_ota_write(buf, n);
        if (r != ESP_OK) { ESP_LOGE(TAG, "write: %s", esp_err_to_name(r)); esp_hosted_slave_ota_end(); if (cb) cb(0, "write failed", ctx); return r; }
        off += n;
        if (cb) {
            int pct = (int)((uint64_t)off * 100 / fw_size);
            char m[32]; snprintf(m, sizeof(m), "flashing C6 %d%%", pct);
            cb(pct, m, ctx);
        }
    }

    r = esp_hosted_slave_ota_end();
    if (r != ESP_OK) { ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(r)); if (cb) cb(100, "end failed", ctx); return r; }

    ESP_LOGI(TAG, "C6 OTA write complete (%u bytes)", (unsigned)fw_size);
    if (cb) cb(100, "written — activating", ctx);
    return ESP_OK;
}

/* Read the app-elf SHA256 of the slave image staged in partition `label`.
 * The esp_app_desc_t sits right after the image + first-segment headers (0x20).
 * Returns ESP_OK and fills sha[32] on a valid app image. */
static esp_err_t staged_sha(const char *label, uint8_t sha[32])
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, label);
    if (!p) return ESP_ERR_NOT_FOUND;
    esp_app_desc_t desc;
    esp_err_t r = esp_partition_read(p, sizeof(esp_image_header_t) +
                                     sizeof(esp_image_segment_header_t),
                                     &desc, sizeof(desc));
    if (r != ESP_OK) return r;
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) return ESP_ERR_INVALID_STATE;
    memcpy(sha, desc.app_elf_sha256, 32);
    return ESP_OK;
}

bool hc_c6ota_stale(const char *label)
{
    uint8_t staged[32];
    if (staged_sha(label, staged) != ESP_OK) return false;  /* no valid image to offer */
    uint8_t installed[32]; size_t len = sizeof(installed);
    nvs_handle_t h;
    bool have = false;
    if (nvs_open("hc_c6", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "sha", installed, &len) == ESP_OK && len == 32) have = true;
        nvs_close(h);
    }
    return !have || memcmp(installed, staged, 32) != 0;
}

void hc_c6ota_mark_installed(const char *label)
{
    uint8_t staged[32];
    if (staged_sha(label, staged) != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open("hc_c6", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "sha", staged, 32);
        nvs_commit(h);
        nvs_close(h);
    }
}

void hc_c6ota_activate_and_restart(void)
{
    esp_err_t r = esp_hosted_slave_ota_activate();
    ESP_LOGW(TAG, "activate: %s — restarting P4 to resync with C6", esp_err_to_name(r));
    /* The activate response has already been received (r above), so restart the
     * P4 promptly — before the C6 reboots and drops the SDIO link. Waiting here
     * lets the host's SDIO transport hit TRANSPORT_FAILURE and abort() as the
     * link dies; a quick restart beats that window and reboots cleanly onto the
     * freshly-activated slave (the P4 resets the C6 via GPIO15 on boot anyway). */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}
