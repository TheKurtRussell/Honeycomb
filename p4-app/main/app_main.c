/*
 * Honeycomb — P4 application entry point
 *
 * Wires the subsystems together:
 *   board -> storage -> explorer(+UI sinks) -> mode manager(link+C6) -> flasher -> UI
 *
 * Everything is callback-driven: the C6 pushes frames up through the mode
 * manager into the explorer, which dissects and forwards to the UI and (when
 * capturing) to the pcap writer on SD.
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "hc_board.h"
#include "hc_storage.h"
#include "hc_explorer.h"
#include "hc_mode_manager.h"
#include "hc_flasher.h"
#include "hc_c6updater.h"
#include "hc_ui.h"

#define TAG "honeycomb"

/* explorer -> UI */
static void on_row(const hc_dissected_t *row, void *ctx)    { hc_ui_add_frame(row); }
static void on_energy(const hc_energy_result_t *e, void *c) { hc_ui_set_energy(e); }
static void on_scan(const hc_scan_result_t *s, void *c)     { hc_ui_add_scan(s); }

/* mode manager -> UI */
static void on_mode(hc_radio_mode_t m, bool ok, uint8_t reason, void *c)
{
    ESP_LOGI(TAG, "mode -> %d ok=%d", m, ok);
    hc_ui_set_mode(m, ok);
}

/* flasher -> UI */
static void on_flash(const hc_flash_progress_t *p, void *c) { hc_ui_set_flash_progress(p); }

void app_main(void)
{
    ESP_LOGI(TAG, "Honeycomb P4 app 0.1.0");
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(hc_board_init());        /* display BSP (headless until bound) */
    hc_storage_mount();                      /* SD; non-fatal if absent */

    hc_ui_init(hc_board_lvgl_display());     /* bound to the BSP display */

    hc_explorer_callbacks_t ecb = { .on_row = on_row, .on_energy = on_energy,
                                    .on_scan = on_scan };
    ESP_ERROR_CHECK(hc_explorer_init(&ecb));

    hc_flasher_init(on_flash, NULL);
    /* hc_flasher_start() is invoked from the Bench tab when a target is plugged
     * in; starting the USB host unconditionally at boot is optional. */

    /* NOTE: ESP-Hosted WiFi (C6 over SDIO) is temporarily reverted. It brought
     * the SDIO transport up with the right Tab5 pins, but ESP-Hosted claims the
     * P4's SDMMC controller (shared with the microSD on this board), which then
     * makes the SD mount fail and assert -> boot loop. Re-enable only once the
     * C6/SD SDMMC coexistence is handled. For now the C6 stays offline; the P4
     * flasher / serial bench / file browser / UI all run without it. */
    esp_err_t e = hc_mode_manager_start(on_mode, NULL);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "C6 link not up (%s)", esp_err_to_name(e));
        ESP_LOGW(TAG, "continuing without C6 — flash it via the M5 downloader (Plan B)");
    }

    ESP_LOGI(TAG, "ready");
}
