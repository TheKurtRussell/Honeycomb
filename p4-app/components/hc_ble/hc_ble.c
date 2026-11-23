#include "hc_ble.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_hosted_bt_host_stack.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#define TAG      "hc_ble"
#define MAXDEV   48

static hc_ble_dev_t     s_dev[MAXDEV];
static int              s_ndev;
static SemaphoreHandle_t s_lock;
static volatile bool    s_ready;
static volatile bool    s_scanning;

/* Find-or-add by address, then update RSSI (and name if this report has one). */
static void add_report(const struct ble_gap_disc_desc *d)
{
    char name[32] = "";
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 && f.name_len) {
        int n = f.name_len < 31 ? f.name_len : 31;
        memcpy(name, f.name, n);
        name[n] = 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i;
    for (i = 0; i < s_ndev; i++)
        if (memcmp(s_dev[i].addr, d->addr.val, 6) == 0) break;
    if (i == s_ndev && s_ndev < MAXDEV) {
        s_ndev++;
        memcpy(s_dev[i].addr, d->addr.val, 6);
        s_dev[i].name[0] = 0;
    }
    if (i < MAXDEV) {
        s_dev[i].rssi = d->rssi;
        if (name[0]) strlcpy(s_dev[i].name, name, sizeof(s_dev[i].name));
    }
    xSemaphoreGive(s_lock);
}

static int gap_cb(struct ble_gap_event *e, void *arg)
{
    if (e->type == BLE_GAP_EVENT_DISC) {
        add_report(&e->disc);
    } else if (e->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_scanning = false;
        ESP_LOGI(TAG, "scan complete (%d devices)", s_ndev);
    }
    return 0;
}

void hc_ble_scan(uint32_t ms)
{
    if (!s_ready || s_scanning) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ndev = 0;
    xSemaphoreGive(s_lock);

    uint8_t own = 0;
    if (ble_hs_id_infer_auto(0, &own) != 0) return;
    struct ble_gap_disc_params p = { .passive = 1, .filter_duplicates = 0 };
    s_scanning = true;
    if (ble_gap_disc(own, ms, &p, gap_cb, NULL) != 0) {
        s_scanning = false;
        ESP_LOGW(TAG, "ble_gap_disc failed");
    }
}

bool hc_ble_scanning(void) { return s_scanning; }
bool hc_ble_ready(void)    { return s_ready; }

int hc_ble_get(hc_ble_dev_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_ndev < max ? s_ndev : max;
    memcpy(out, s_dev, n * sizeof(hc_ble_dev_t));
    xSemaphoreGive(s_lock);

    /* strongest signal first (small n, simple insertion-ish sort) */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (out[b].rssi > out[a].rssi) {
                hc_ble_dev_t t = out[a]; out[a] = out[b]; out[b] = t;
            }
    return n;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_ready = true;
    ESP_LOGI(TAG, "BLE host ready (controller synced)");
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool hc_ble_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    esp_hosted_bt_host_stack_cfg_t bt = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    if (esp_hosted_bt_host_stack_setup(&bt) != ESP_OK) {
        ESP_LOGW(TAG, "host stack setup failed (needs the BLE-capable C6 slave)");
        return false;
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    return true;
}
