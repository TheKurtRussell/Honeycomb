/*
 * Honeycomb — C6 coprocessor (ESP-Hosted slave) + 802.15.4 scanner
 *
 * The stock ESP-Hosted coprocessor (Wi-Fi + BLE over SDIO), extended with a
 * time-sliced IEEE 802.15.4 (Zigbee/Thread/Matter) activity scan that the P4
 * host triggers over the Peer Data Transfer (Custom RPC) channel.
 *
 * Flow:
 *   host --(MSG_Z154_SCAN_REQ)--> CP   : one optional byte = dwell per channel
 *                                         in 10ms units (0 => default)
 *   CP worker task borrows the radio, sweeps channels 11..26 in promiscuous RX,
 *   counts frames + tracks RSSI/LQI per channel, then:
 *   CP --(MSG_Z154_SCAN_RESULT)--> host : [status][nchan] + nchan*{ch,frames,rssi,lqi}
 *
 * The worker ALWAYS sends a result (status=1 if the radio couldn't be enabled),
 * so a transport problem is distinguishable from a radio problem in the logs.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "eh_cp_feat_peer_data.h"
#include "esp_ieee802154.h"

static const char *TAG = "c6_hosted";

#define MSG_Z154_SCAN_REQ     0x5401u
#define MSG_Z154_SCAN_RESULT  0x5402u

#define CH_LO 11
#define CH_HI 26
#define NCHAN (CH_HI - CH_LO + 1)
#define DEFAULT_DWELL_MS 120

/* Per-channel accumulators, updated from the RX-done callback. */
static volatile uint16_t s_frames[NCHAN];
static volatile int8_t   s_rssi[NCHAN];
static volatile uint8_t  s_lqi[NCHAN];
static TaskHandle_t      s_scan_task;
static volatile uint8_t  s_dwell_10ms;   /* requested dwell, 10ms units */

/* IEEE 802.15.4 RX-done hook (weak symbol in the driver). Runs in driver ctx —
 * keep it tiny: tally the frame for the current channel, then release it. */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    if (info) {
        int idx = (int)info->channel - CH_LO;
        if (idx >= 0 && idx < NCHAN) {
            if (s_frames[idx] < 0xFFFF) s_frames[idx]++;
            if (info->rssi > s_rssi[idx]) s_rssi[idx] = info->rssi;
            s_lqi[idx] = info->lqi;
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

static void z154_scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Wait for a scan request. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t dwell = s_dwell_10ms ? (s_dwell_10ms * 10u) : DEFAULT_DWELL_MS;
        memset((void *)s_frames, 0, sizeof(s_frames));
        for (int i = 0; i < NCHAN; i++) { s_rssi[i] = -128; s_lqi[i] = 0; }

        uint8_t status = 0;
        if (esp_ieee802154_enable() != ESP_OK) {
            status = 1;
            ESP_LOGW(TAG, "802.15.4 enable failed");
        } else {
            esp_ieee802154_set_promiscuous(true);
            esp_ieee802154_set_rx_when_idle(true);
            for (int ch = CH_LO; ch <= CH_HI; ch++) {
                esp_ieee802154_set_channel(ch);
                esp_ieee802154_receive();
                vTaskDelay(pdMS_TO_TICKS(dwell));
            }
            esp_ieee802154_disable();
        }

        /* Pack result: [status][nchan] + nchan*{ch,frames8,rssi,lqi}. */
        uint8_t buf[2 + NCHAN * 4];
        buf[0] = status;
        buf[1] = NCHAN;
        for (int i = 0; i < NCHAN; i++) {
            uint8_t *r = &buf[2 + i * 4];
            r[0] = (uint8_t)(CH_LO + i);
            r[1] = s_frames[i] > 255 ? 255 : (uint8_t)s_frames[i];
            r[2] = (uint8_t)s_rssi[i];
            r[3] = s_lqi[i];
        }
        esp_err_t e = eh_cp_feat_peer_data_send(MSG_Z154_SCAN_RESULT, buf, sizeof(buf));
        ESP_LOGI(TAG, "802.15.4 scan done (status=%d) send=%d", status, e);
    }
}

/* Host asked for a scan — stash the dwell and wake the worker. Keep this fast;
 * it runs in RPC RX context. */
static void on_scan_req(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)ctx;
    s_dwell_10ms = (len >= 1) ? data[0] : 0;
    if (s_scan_task) xTaskNotifyGive(s_scan_task);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Honeycomb C6 ESP-Hosted slave (Wi-Fi + BLE + 802.15.4 scan)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 802.15.4 scan worker + Peer Data request handler. Peer Data auto-inits
     * via CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA; we just register our msg_id. */
    xTaskCreate(z154_scan_task, "z154_scan", 4096, NULL, 4, &s_scan_task);
    esp_err_t pe = eh_cp_feat_peer_data_register_callback(MSG_Z154_SCAN_REQ, on_scan_req, NULL);
    ESP_LOGI(TAG, "802.15.4 scan request handler registered (%d)", pe);
}
