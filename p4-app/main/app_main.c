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

/* BLE via the C6 (NimBLE host on the P4, controller on the C6 over SDIO). */
#include "esp_hosted_bt_host_stack.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

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

/* --- BLE (NimBLE observer) over the C6, Stage 3a: one boot scan to serial --- */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_gap_disc_desc *d = &event->disc;
    char name[32] = "";
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 && f.name_len) {
        int n = f.name_len < 31 ? f.name_len : 31;
        memcpy(name, f.name, n);
        name[n] = 0;
    }
    ESP_LOGI(TAG, "BLE %3d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %s",
             d->rssi, d->addr.val[5], d->addr.val[4], d->addr.val[3],
             d->addr.val[2], d->addr.val[1], d->addr.val[0], name);
    return 0;
}

static void ble_on_sync(void)
{
    uint8_t own = 0;
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &own) != 0) return;
    struct ble_gap_disc_params p = { .passive = 1, .filter_duplicates = 1 };
    ESP_LOGI(TAG, "BLE up over C6 — scanning 8s...");
    ble_gap_disc(own, 8000, &p, ble_gap_event, NULL);   /* 8s observer scan */
}

static void ble_host_task(void *param)
{
    nimble_port_run();                 /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Bring up the NimBLE host bound to the C6's controller over ESP-Hosted HCI.
 * Returns true if the stack started (scan runs from ble_on_sync). */
static bool ble_bringup(void)
{
    esp_hosted_bt_host_stack_cfg_t bt = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    if (esp_hosted_bt_host_stack_setup(&bt) != ESP_OK) {
        ESP_LOGW(TAG, "BLE: host stack setup failed (needs the 3.1.0 C6 slave)");
        return false;
    }
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "BLE: nimble_port_init failed"); return false; }
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Honeycomb P4 app 0.1.0");
    ESP_ERROR_CHECK(nvs_flash_init());
    c6fw_report();

    ESP_ERROR_CHECK(hc_board_init());        /* display BSP (headless until bound) */

    /* C6 radio over ESP-Hosted, then SD. The C6 (SDIO slot 1) and the microSD
     * (slot 0) share the P4's one SDMMC controller, so order matters: power the
     * C6, bring ESP-Hosted up on slot 1, THEN mount the SD on slot 0. ESP-Hosted
     * auto-init is disabled (sdkconfig) so this ordering is ours to control. */
    hc_board_c6_power(true);
    vTaskDelay(pdMS_TO_TICKS(100));          /* let the C6 boot before we probe it */
    bool c6_up = c6_wifi_bringup();          /* non-fatal if the C6 doesn't answer */
    bool ble_up = c6_up ? ble_bringup() : false;  /* BLE needs the C6 link up first */

    hc_storage_mount();                      /* SD on slot 0; non-fatal if absent */

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
