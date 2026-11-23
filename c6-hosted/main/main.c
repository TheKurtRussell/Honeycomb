/*
 * Honeycomb — C6 coprocessor (ESP-Hosted slave)
 *
 * This is the firmware that runs ON the Tab5's on-board ESP32-C6. It is the
 * stock ESP-Hosted coprocessor with Wi-Fi + BLE enabled, carried to the P4 host
 * over SDIO. ESP-Hosted's coprocessor role auto-starts (transport + RPC) before
 * app_main, so app_main only needs to bring up NVS and the default event loop.
 *
 * The P4 flashes this onto the C6 via host-driven OTA (esp_hosted_cp_ota_*),
 * replacing the factory WLAN-only slave so BLE/HCI becomes available too.
 */
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

static const char *TAG = "c6_hosted";

void app_main(void)
{
    ESP_LOGI(TAG, "Honeycomb C6 ESP-Hosted slave (Wi-Fi + BLE)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
}
