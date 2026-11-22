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
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

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

    /* C6 radio via ESP-Hosted (SDIO) — the real Tab5 mechanism. esp_wifi_remote
     * forwards esp_wifi onto the C6 slave over SDIO (pins from the Tab5 board
     * profile). This replaces the old custom UART handshake (hc_mode_manager),
     * which spoke a protocol the factory C6 doesn't run. Phase 1 proof: bring
     * WiFi up on the factory slave and do one scan, logged to serial. If the
     * slave firmware version doesn't match this host, esp_wifi_init reports it
     * here — that's our signal to do the Phase-2 SDIO reflash. */
    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) e = esp_event_loop_create_default();
    if (e == ESP_OK) { esp_netif_create_default_wifi_sta(); }
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&wcfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init via C6 failed: %s", esp_err_to_name(e));
        ESP_LOGW(TAG, "C6 WiFi not up — likely a slave-firmware version mismatch "
                      "(factory slave is 1.4.1); Phase-2 SDIO reflash needed");
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "C6 WiFi up over SDIO — scanning...");
        if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            ESP_LOGI(TAG, "scan: %u APs", n);
            uint16_t got = n < 20 ? n : 20;
            static wifi_ap_record_t recs[20];
            if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK)
                for (int i = 0; i < got; i++)
                    ESP_LOGI(TAG, "  %3d dBm  ch%-2d  %s",
                             recs[i].rssi, recs[i].primary, (char *)recs[i].ssid);
        }
    }

    ESP_LOGI(TAG, "ready");
}
