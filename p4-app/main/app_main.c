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
#include "esp_hosted.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc_ble.h"

#include "hc_board.h"
#include "hc_storage.h"
#include "hc_scanlog.h"
#include "hc_z154.h"
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

/* mode manager -> UI (kept for the Phase-2 C6 sniffer control path) */
__attribute__((unused))
static void on_mode(hc_radio_mode_t m, bool ok, uint8_t reason, void *c)
{
    ESP_LOGI(TAG, "mode -> %d ok=%d", m, ok);
    hc_ui_set_mode(m, ok);
}

/* Bring the C6 radio up over ESP-Hosted (SDIO slot 1) and do one Wi-Fi scan as
 * proof of life. Safe to fail: on any error we log and return, and the rest of
 * the app (bench, SD, UI) runs fine without the radio. Must be called AFTER the
 * C6 is powered and BEFORE the SD mounts (SD is slot 0 of the same controller;
 * ESP-Hosted claims slot 1 here). */
static bool c6_wifi_bringup(void)
{
    int r = esp_hosted_init();
    if (r == 0) r = esp_hosted_connect_to_slave();
    if (r != 0) {
        ESP_LOGW(TAG, "C6 link not up (esp_hosted rc=%d) — continuing without Wi-Fi", r);
        return false;
    }

    esp_hosted_app_desc_t desc = { 0 };
    if (esp_hosted_get_coprocessor_app_desc(&desc) == ESP_OK)
        ESP_LOGW(TAG, "C6 slave: %s  ver=%s  idf=%s", desc.project_name, desc.version, desc.idf_ver);

    if (esp_netif_init() != ESP_OK) return false;
    if (esp_event_loop_create_default() != ESP_OK) return false;
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wcfg) != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init failed"); return false; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    ESP_LOGI(TAG, "C6 Wi-Fi up over SDIO — scanning...");
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        ESP_LOGI(TAG, "scan: %u APs", n);
        uint16_t got = n < 20 ? n : 20;
        static wifi_ap_record_t recs[20];
        if (got && esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK)
            for (int i = 0; i < got; i++)
                ESP_LOGI(TAG, "  %3d dBm  ch%-2d  %s",
                         recs[i].rssi, recs[i].primary, (char *)recs[i].ssid);
    }
    return true;
}

/* flasher -> UI */
static void on_flash(const hc_flash_progress_t *p, void *c) { hc_ui_set_flash_progress(p); }

/* Report the staged C6 slave image in the c6fw partition (Stage 2a check).
 * A valid ESP app image begins with magic byte 0xE9. This verifies the web
 * flasher delivered the slave image before we build the OTA that streams it. */
static void c6fw_report(void)
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "c6fw");
    if (!p) { ESP_LOGW(TAG, "c6fw partition not found"); return; }
    uint8_t magic = 0;
    esp_partition_read(p, 0, &magic, 1);
    ESP_LOGI(TAG, "c6fw: %lu KB partition, staged image magic=0x%02x (%s)",
             (unsigned long)(p->size / 1024), magic,
             magic == 0xE9 ? "valid app image" : "EMPTY/invalid");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Honeycomb P4 app 0.1.0");
    ESP_ERROR_CHECK(nvs_flash_init());
    c6fw_report();

    ESP_ERROR_CHECK(hc_board_init());        /* display BSP (headless until bound) */
    hc_ui_splash(hc_board_lvgl_display());    /* themed loading screen during bring-up */

    /* C6 radio over ESP-Hosted, then SD. The C6 (SDIO slot 1) and the microSD
     * (slot 0) share the P4's one SDMMC controller, so order matters: power the
     * C6, bring ESP-Hosted up on slot 1, THEN mount the SD on slot 0. ESP-Hosted
     * auto-init is disabled (sdkconfig) so this ordering is ours to control. */
    hc_board_c6_power(true);
    vTaskDelay(pdMS_TO_TICKS(100));          /* let the C6 boot before we probe it */
    bool c6_up = c6_wifi_bringup();          /* non-fatal if the C6 doesn't answer */
    bool ble_up = c6_up ? hc_ble_start() : false;  /* BLE needs the C6 link up first */

    hc_storage_mount();                      /* SD on slot 0; non-fatal if absent */
    hc_scanlog_init();                       /* scan logbook (CSV on SD) */
    if (c6_up) hc_z154_init();               /* 802.15.4 scan over peer-data RPC */

    hc_ui_init(hc_board_lvgl_display());     /* bound to the BSP display */
    hc_ui_set_radio(c6_up, ble_up);          /* header dots reflect the live radios */

    hc_explorer_callbacks_t ecb = { .on_row = on_row, .on_energy = on_energy,
                                    .on_scan = on_scan };
    ESP_ERROR_CHECK(hc_explorer_init(&ecb));

    hc_flasher_init(on_flash, NULL);
    /* hc_flasher_start() is invoked from the Bench tab when a target is plugged
     * in; starting the USB host unconditionally at boot is optional. */

    ESP_LOGI(TAG, "ready");
}
