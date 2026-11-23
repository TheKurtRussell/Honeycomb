#include "hc_ble.h"
#include "hc_scanlog.h"

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
static volatile uint32_t s_adv_total;   /* advertisement reports seen (traffic meter) */

/* Find-or-add by address, then update RSSI/identity from this report. */
static void add_report(const struct ble_gap_disc_desc *d)
{
    s_adv_total++;

    char name[32] = "";
    uint16_t company = 0xFFFF, appearance = 0, svc_uuid = 0;
    uint8_t  tracker = HC_TRACK_NONE;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0) {
        if (f.name_len) {
            int n = f.name_len < 31 ? f.name_len : 31;
            memcpy(name, f.name, n);
            name[n] = 0;
        }
        if (f.mfg_data && f.mfg_data_len >= 2)
            company = f.mfg_data[0] | (f.mfg_data[1] << 8);   /* little-endian company ID */
        if (f.appearance_is_present)
            appearance = f.appearance;

        /* First advertised 16-bit UUID (service list or service-data). */
        if (f.num_uuids16 > 0)
            svc_uuid = ble_uuid_u16(&f.uuids16[0].u);
        if (!svc_uuid && f.svc_data_uuid16 && f.svc_data_uuid16_len >= 2)
            svc_uuid = f.svc_data_uuid16[0] | (f.svc_data_uuid16[1] << 8);

        /* Item-tracker classification (counter-surveillance). */
        if (company == 0x004C && f.mfg_data_len >= 3 && f.mfg_data[2] == 0x12)
            tracker = HC_TRACK_APPLE;              /* Apple Find My (separated AirTag) */
        else if (svc_uuid == 0xFEED || svc_uuid == 0xFEEC)
            tracker = HC_TRACK_TILE;               /* Tile */
        else if (svc_uuid == 0xFD5A || svc_uuid == 0xFD59)
            tracker = HC_TRACK_SAMSUNG;            /* Samsung SmartTag */
        else if (svc_uuid == 0xFCF1 || svc_uuid == 0xFE33)
            tracker = HC_TRACK_GENERIC;            /* Google / Chipolo finder */
    }
    /* Legacy adv event types (BT spec): 0=ADV_IND, 1=ADV_DIRECT_IND are the
     * connectable ones; 2=SCAN_IND, 3=NONCONN_IND, 4=SCAN_RSP are not. */
    bool conn = (d->event_type == 0 || d->event_type == 1);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i;
    for (i = 0; i < s_ndev; i++)
        if (memcmp(s_dev[i].addr, d->addr.val, 6) == 0) break;
    if (i == s_ndev && s_ndev < MAXDEV) {
        s_ndev++;
        memcpy(s_dev[i].addr, d->addr.val, 6);
        s_dev[i].name[0]   = 0;
        s_dev[i].company_id = 0xFFFF;
        s_dev[i].appearance = 0;
        s_dev[i].svc_uuid   = 0;
        s_dev[i].tracker    = HC_TRACK_NONE;
    }
    if (i < MAXDEV) {
        s_dev[i].rssi      = d->rssi;
        s_dev[i].addr_type = d->addr.type;
        s_dev[i].connectable = conn;
        if (name[0])            strlcpy(s_dev[i].name, name, sizeof(s_dev[i].name));
        if (company != 0xFFFF)  s_dev[i].company_id = company;
        if (appearance)         s_dev[i].appearance = appearance;
        if (svc_uuid)           s_dev[i].svc_uuid = svc_uuid;
        if (tracker != HC_TRACK_NONE) s_dev[i].tracker = tracker;
    }
    xSemaphoreGive(s_lock);

    /* Logbook: record every unique sighting (dedups + appends to SD itself).
     * Done outside s_lock so SD I/O never stalls the BLE device table. */
    hc_scanlog_ble(d->addr.val, d->rssi, company, name, tracker);
}

/* BLE SIG assigned company identifiers (the common ones you'll see around). */
const char *hc_ble_vendor(uint16_t cid)
{
    switch (cid) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x0075: return "Samsung";
        case 0x00E0: return "Google";
        case 0x0087: return "Garmin";
        case 0x0157: return "Amazfit/Huami";
        case 0x038F: return "Xiaomi";
        case 0x0059: return "Nordic";
        case 0x05A7: return "Sonos";
        case 0x00D2: return "Logitech";
        case 0x0171: return "Amazon";
        case 0x0085: return "BLZ/Tile";
        case 0x0499: return "Ruuvi";
        case 0x0180: return "Fitbit";
        case 0x02E5: return "Espressif";
        case 0x0117: return "Bose";
        case 0x004F: return "APT/Qualcomm";
        case 0x03DA: return "Govee";
        case 0x0646: return "JBL/Harman";
        default:     return "";
    }
}

const char *hc_ble_tracker_name(uint8_t t)
{
    switch (t) {
        case HC_TRACK_APPLE:   return "Apple Find My";
        case HC_TRACK_TILE:    return "Tile";
        case HC_TRACK_SAMSUNG: return "Samsung SmartTag";
        case HC_TRACK_GENERIC: return "finder tag";
        default:               return "";
    }
}

uint32_t hc_ble_adv_total(void) { return s_adv_total; }

/* GAP appearance categories (top 10 bits select the category). */
const char *hc_ble_appearance_name(uint16_t a)
{
    if (!a) return "";
    switch (a >> 6) {            /* category = appearance / 64 */
        case 1:   return "phone";
        case 2:   return "computer";
        case 3:   return "watch";
        case 4:   return "clock";
        case 5:   return "display";
        case 6:   return "remote";
        case 7:   return "glasses";
        case 8:   return "tag";
        case 9:   return "keyring";
        case 10:  return "media player";
        case 11:  return "barcode";
        case 15:  return "thermometer";
        case 16:  return "heart rate";
        case 17:  return "blood pressure";
        case 18:  return "HID";
        case 19:  return "glucose";
        case 20:  return "running";
        case 21:  return "cycling";
        case 49:  return "pulse ox";
        case 51:  return "weight scale";
        case 64:  return "earbuds";
        case 65:  return "headset";
        case 66:  return "speaker";
        default:  return "device";
    }
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

void hc_ble_stop(void)
{
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
}

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

int hc_ble_find(const uint8_t *addr)
{
    int r = -128;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_ndev; i++)
        if (memcmp(s_dev[i].addr, addr, 6) == 0) { r = s_dev[i].rssi; break; }
    xSemaphoreGive(s_lock);
    return r;
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
