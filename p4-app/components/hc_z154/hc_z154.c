/* Honeycomb — 802.15.4 scan, host side (see hc_z154.h) */
#include "hc_z154.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "eh_host_feat_peer_data.h"

#define TAG "hc_z154"

#define MSG_Z154_SCAN_REQ     0x5401u
#define MSG_Z154_SCAN_RESULT  0x5402u
#define Z154_MAXCH            16

static hc_z154_chan_t    s_ch[Z154_MAXCH];
static int               s_nch;
static uint8_t           s_status = 0xFF;
static uint32_t          s_last_ms;
static volatile bool     s_scanning;
static SemaphoreHandle_t s_lock;
static uint8_t           s_req_dwell;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Result from the C6: [status][nchan] + nchan*{ch,frames,rssi,lqi}. */
static void on_result(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)ctx;
    if (!data || len < 2) { s_scanning = false; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = data[0];
    int n = data[1];
    if (n > Z154_MAXCH) n = Z154_MAXCH;
    int avail = (int)(len - 2) / 4;
    if (n > avail) n = avail;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = &data[2 + i * 4];
        s_ch[i].channel = r[0];
        s_ch[i].frames  = r[1];
        s_ch[i].rssi    = (int8_t)r[2];
        s_ch[i].lqi     = r[3];
    }
    s_nch = n;
    s_last_ms = now_ms();
    xSemaphoreGive(s_lock);
    s_scanning = false;
    ESP_LOGI(TAG, "scan result: status=%d channels=%d", s_status, n);
}

void hc_z154_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    esp_err_t e = eh_host_peer_data_register(MSG_Z154_SCAN_RESULT, on_result, NULL);
    ESP_LOGI(TAG, "802.15.4 result handler registered (%d)", e);
}

/* The send blocks until the CP acks, so do it off the caller's thread. */
static void scan_send_task(void *arg)
{
    (void)arg;
    uint8_t dwell = s_req_dwell;
    esp_err_t e = eh_host_peer_data_send(MSG_Z154_SCAN_REQ, &dwell, 1);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan request send failed (%d)", e);
        s_scanning = false;      /* let the UI recover */
    }
    vTaskDelete(NULL);
}

bool hc_z154_scan(uint8_t dwell_10ms)
{
    if (!s_lock) hc_z154_init();
    if (s_scanning) return false;
    s_scanning = true;
    s_req_dwell = dwell_10ms;
    if (xTaskCreate(scan_send_task, "z154_req", 4096, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        return false;
    }
    return true;
}

bool     hc_z154_scanning(void)    { return s_scanning; }
bool     hc_z154_have_result(void) { return s_status != 0xFF; }
uint8_t  hc_z154_status(void)      { return s_status; }
uint32_t hc_z154_last_ms(void)     { return s_last_ms; }

int hc_z154_get(hc_z154_chan_t *out, int max)
{
    if (!out || max <= 0 || !s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_nch < max ? s_nch : max;
    memcpy(out, s_ch, n * sizeof(hc_z154_chan_t));
    xSemaphoreGive(s_lock);
    return n;
}
