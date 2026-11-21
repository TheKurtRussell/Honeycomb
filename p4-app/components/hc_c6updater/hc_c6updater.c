/*
 * Honeycomb — C6 updater implementation (esp-serial-flasher)
 *
 * Targets esp-serial-flasher v2.x (handle-based API). Two interface choices:
 *
 *   UART  — simplest/most reliable to bring up. Needs the C6's download UART
 *           (TX0/RX0) reachable on P4 GPIOs. On the Tab5 the cleanest option is
 *           the same two M5-Bus pins the link already uses.
 *   SDIO  — no extra wires (reuses the SDIO2 bus), but marked experimental for
 *           P4->C6 in the library. Preferred once proven.
 *
 * Pick with SERIAL_FLASHER_INTERFACE_* in the component config. We default to
 * UART for first bring-up; flip to SDIO later.
 *
 * NOTE: the exact port-init call and the C6 RESET/BOOT GPIOs must match your
 * board. Espressif's P4->C6 examples use two host GPIOs for RESET and BOOT; on
 * the Tab5 those are HC_PIN_C6_RESET and HC_PIN_C6_IO2 (VERIFY against the
 * schematic — IO2 must be the C6 strapping/boot pin for download mode).
 */
#include "hc_c6updater.h"
#include "hc_board.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* esp-serial-flasher public API (added via idf_component.yml). The include is
 * guarded so the project still configures before the component is fetched. */
#if __has_include("esp_loader.h")
#include "esp_loader.h"
#include "esp_loader_io.h"
#define HAVE_SERIAL_FLASHER 1
#else
#define HAVE_SERIAL_FLASHER 0
#endif

#define TAG "hc_c6upd"
#define DEFAULT_SD_PATH "/sdcard/honeycomb/c6_service.bin"
#define FLASH_CHUNK 1024

bool hc_c6updater_should_offer(bool c6_handshake_ok, const char *sd_path)
{
    if (c6_handshake_ok) return false;             /* C6 already works */
    struct stat st;
    return stat(sd_path ? sd_path : DEFAULT_SD_PATH, &st) == 0;
}

#if HAVE_SERIAL_FLASHER
static esp_err_t flash_image(FILE *f, long size, hc_c6upd_progress_cb_t cb, void *ctx)
{
    esp_loader_flash_cfg_t cfg = {
        .offset     = 0,            /* merged image starts at 0x0 */
        .image_size = (uint32_t)size,
        .block_size = FLASH_CHUNK,
    };
    if (esp_loader_flash_start(&cfg) != ESP_LOADER_SUCCESS) {
        if (cb) cb(-1, "flash_start failed", ctx);
        return ESP_FAIL;
    }

    uint8_t buf[FLASH_CHUNK];
    long done = 0; size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (esp_loader_flash_write(buf, r) != ESP_LOADER_SUCCESS) {
            if (cb) cb(-1, "flash_write failed", ctx);
            return ESP_FAIL;
        }
        done += r;
        if (cb) cb((int)(100 * done / size), "writing", ctx);
    }
    return ESP_OK;
}
#endif

esp_err_t hc_c6updater_run(const char *sd_path, hc_c6upd_progress_cb_t cb, void *ctx)
{
    const char *path = sd_path ? sd_path : DEFAULT_SD_PATH;
#if !HAVE_SERIAL_FLASHER
    ESP_LOGE(TAG, "esp-serial-flasher not linked; add espressif/esp-serial-flasher");
    if (cb) cb(-1, "flasher lib missing", ctx);
    return ESP_ERR_NOT_SUPPORTED;
#else
    FILE *f = fopen(path, "rb");
    if (!f) { if (cb) cb(-1, "no c6 image on SD", ctx); return ESP_FAIL; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    ESP_LOGI(TAG, "flashing C6 from %s (%ld bytes)", path, size);
    if (cb) cb(0, "connecting to C6", ctx);

    /* Put the C6 into serial-download mode: BOOT low, pulse RESET.
     * (hc_board owns the GPIOs.) esp-serial-flasher's port layer can also do
     * this via esp_loader_reset_target()/esp_loader_connect() reset hooks once
     * the RESET/BOOT pins are registered in the port config. */
    esp_loader_connect_args_t connect = ESP_LOADER_CONNECT_DEFAULT();

    /* TODO(port): initialise the interface the library will use. For UART:
     *   loader_port_esp32_uart_init(&uart_cfg); with C6 TX/RX + RESET/BOOT pins.
     * For SDIO (experimental P4->C6): the SDIO port init from the library's
     * esp32_sdio_example. Wire RESET=HC_PIN_C6_RESET, BOOT=HC_PIN_C6_IO2. */

    if (esp_loader_connect_with_stub(&connect) != ESP_LOADER_SUCCESS) {
        fclose(f); if (cb) cb(-1, "C6 connect failed", ctx);
        return ESP_FAIL;
    }
    if (cb) cb(5, "connected, erasing+writing", ctx);

    esp_err_t e = flash_image(f, size, cb, ctx);
    fclose(f);

    esp_loader_reset_target();     /* boot the C6 into the freshly flashed app */
    if (e == ESP_OK && cb) cb(100, "C6 updated", ctx);
    vTaskDelay(pdMS_TO_TICKS(200));
    return e;
#endif
}
