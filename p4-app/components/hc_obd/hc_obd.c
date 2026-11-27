/* Honeycomb — car diagnostics over a BLE ELM327 dongle (see hc_obd.h)
 *
 * Increment 1: prove the transport. Connect to a BLE ELM327, auto-detect its
 * serial GATT characteristics (notify = RX, write = TX), subscribe, run the
 * ELM327 handshake, then poll the Ioniq 5 BMS PIDs. Raw responses are captured
 * and logged so the byte-offset decode can be calibrated on real hardware; a
 * provisional decode of the headline values is applied meanwhile. Read-only.
 */
#include "hc_obd.h"
#include "hc_ble.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

#define TAG "hc_obd"

#define RX_MAX   600
#define RAW_MAX  160

static volatile uint8_t  s_state = HC_OBD_IDLE;
static char              s_status[64] = "idle";
static uint16_t          s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t          s_tx_handle;        /* write (commands to dongle) */
static uint16_t          s_rx_handle;        /* notify (replies from dongle) */
static uint16_t          s_cccd_handle;
static bool              s_tx_norsp;
static TaskHandle_t      s_task;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_resp;             /* given when a '>' prompt arrives */
static char              s_rx[RX_MAX];
static volatile int      s_rxn;
static char              s_raw[RAW_MAX] = ""; /* last reassembled hex, for the UI */

static hc_obd_metrics_t  s_m;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void set_status(const char *s) { strlcpy(s_status, s, sizeof(s_status)); }

uint8_t     hc_obd_state(void)  { return s_state; }
const char *hc_obd_status(void) { return s_status; }
const char *hc_obd_last_raw(void){ return s_raw; }

void hc_obd_get(hc_obd_metrics_t *out)
{
    if (!out) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_m;
    if (s_lock) xSemaphoreGive(s_lock);
}

bool hc_obd_name_is_dongle(const char *n)
{
    if (!n || !n[0]) return false;
    static const char *pats[] = {
        "OBD", "ELM", "VLINK", "VGATE", "ICAR", "VEEPEAK", "OBDII",
        "V-LINK", "IOS-VLINK", "ANDROID-VLINK", "CARISTA", "LELINK", NULL
    };
    char up[40]; int i = 0;
    for (; n[i] && i < 39; i++) up[i] = toupper((unsigned char)n[i]);
    up[i] = 0;
    for (int p = 0; pats[p]; p++) if (strstr(up, pats[p])) return true;
    return false;
}

/* ---- notify RX: accumulate until the ELM327 '>' prompt ---- */
static void rx_feed(const uint8_t *d, int n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool prompt = false;
    for (int i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '>') { prompt = true; continue; }
        if (c == '\r') c = '\n';
        if (s_rxn < RX_MAX - 1) s_rx[s_rxn++] = c;
    }
    s_rx[s_rxn] = 0;
    xSemaphoreGive(s_lock);
    if (prompt) xSemaphoreGive(s_resp);
}

/* ---- ELM327 command/response ---- */
static const char *elm_cmd(const char *cmd, uint32_t timeout_ms)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_tx_handle) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rxn = 0; s_rx[0] = 0;
    xSemaphoreGive(s_lock);
    /* drain any stale signal */
    xSemaphoreTake(s_resp, 0);

    char line[40];
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    int rc = s_tx_norsp
        ? ble_gattc_write_no_rsp_flat(s_conn, s_tx_handle, line, n)
        : ble_gattc_write_flat(s_conn, s_tx_handle, line, n, NULL, NULL);
    if (rc != 0) { ESP_LOGW(TAG, "write '%s' rc=%d", cmd, rc); return NULL; }

    if (xSemaphoreTake(s_resp, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "cmd '%s' timeout", cmd);
        return NULL;
    }
    return s_rx;   /* NUL-terminated, '\n'-separated lines */
}

/* Reassemble an ISO-TP multi-line CAN reply into payload hex bytes after the
 * "62 SS PP" service/PID echo. Tolerant best-effort; raw is logged regardless. */
static int reasm(const char *resp, const char *echo, uint8_t *out, int cap)
{
    if (!resp) return 0;
    /* Pull every hex byte out of every line, skipping an optional 3-char CAN
     * header (e.g. "7EC") and single ISO-TP PCI nibble per frame. */
    uint8_t all[256]; int na = 0;
    const char *p = resp;
    while (*p && na < (int)sizeof(all)) {
        /* one line */
        char ln[128]; int li = 0;
        while (*p && *p != '\n' && li < 127) ln[li++] = *p++;
        ln[li] = 0;
        if (*p == '\n') p++;
        /* collect hex tokens on this line */
        uint8_t fb[64]; int nf = 0;
        for (int i = 0; ln[i] && nf < 64; ) {
            if (isxdigit((unsigned char)ln[i]) && isxdigit((unsigned char)ln[i+1])) {
                fb[nf++] = (uint8_t)strtol((char[]){ln[i], ln[i+1], 0}, NULL, 16);
                i += 2;
            } else i++;
        }
        if (nf == 0) continue;
        /* drop a 7Ex-style header byte if present (0x7E?) */
        int start = 0;
        if ((fb[0] & 0xF0) == 0x70) start = 1;
        /* drop one PCI byte (single 0x0x, first 0x1x, consec 0x2x) */
        if (start < nf) start++;
        for (int i = start; i < nf && na < (int)sizeof(all); i++) all[na++] = fb[i];
    }
    /* find the service/PID echo (e.g. "620101") */
    uint8_t ecb[4]; int ne = 0;
    for (int i = 0; echo[i] && echo[i+1] && ne < 4; i += 2)
        ecb[ne++] = (uint8_t)strtol((char[]){echo[i], echo[i+1], 0}, NULL, 16);
    int off = -1;
    for (int i = 0; i + ne <= na; i++)
        if (memcmp(all + i, ecb, ne) == 0) { off = i + ne; break; }
    if (off < 0) return 0;
    int n = na - off; if (n > cap) n = cap;
    memcpy(out, all + off, n);
    return n;
}

static void store_raw(const uint8_t *b, int n)
{
    char *w = s_raw; int rem = RAW_MAX;
    for (int i = 0; i < n && rem > 3; i++) { int k = snprintf(w, rem, "%02X", b[i]); w += k; rem -= k; }
}

/* Provisional Ioniq 5 decode of 0x220101 payload. Offsets are the commonly
 * cited E-GMP layout and MUST be verified against the raw hex on-device. */
static void decode_0101(const uint8_t *d, int n)
{
    if (n < 24) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_m.soc_bms    = d[5] / 2.0f;                               /* % */
    s_m.dc_current = (int16_t)((d[10] << 8) | d[11]) / 10.0f;   /* A */
    s_m.dc_voltage = ((d[12] << 8) | d[13]) / 10.0f;           /* V */
    s_m.temp_max   = (int8_t)d[14];                            /* C */
    s_m.temp_min   = (int8_t)d[15];                            /* C */
    s_m.cell_max_v = d[25] / 50.0f;                            /* V (if present) */
    s_m.cell_min_v = d[27] / 50.0f;
    if (s_m.cell_max_v > 0 && s_m.cell_min_v > 0)
        s_m.cell_delta_v = s_m.cell_max_v - s_m.cell_min_v;
    s_m.valid = true;
    s_m.updated_ms = now_ms();
    xSemaphoreGive(s_lock);
}

static void decode_0105(const uint8_t *d, int n)
{
    if (n < 28) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_m.soh     = ((d[25] << 8) | d[26]) / 10.0f;   /* % (provisional offset) */
    s_m.aux_12v = d[29] / 10.0f;                     /* V (provisional) */
    xSemaphoreGive(s_lock);
}

static const char *INIT_CMDS[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATSP6", "ATCAF1",
    "ATFCSH7E4", "ATFCSD300000", "ATFCSM1", NULL
};

static void obd_task(void *arg)
{
    (void)arg;
    /* INIT handshake */
    s_state = HC_OBD_INIT; set_status("ELM327 handshake");
    for (int i = 0; INIT_CMDS[i]; i++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) goto done;
        elm_cmd(INIT_CMDS[i], 2000);
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    s_state = HC_OBD_POLLING; set_status("polling BMS");
    uint8_t buf[80];
    while (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        elm_cmd("ATSH7E4", 1500);
        const char *r = elm_cmd("220101", 3000);
        if (r) {
            int n = reasm(r, "620101", buf, sizeof(buf));
            if (n) { store_raw(buf, n); decode_0101(buf, n); ESP_LOGI(TAG, "0101 raw=%s", s_raw); }
            else   ESP_LOGW(TAG, "0101 no payload; resp=%s", r);
        }
        vTaskDelay(pdMS_TO_TICKS(200));

        r = elm_cmd("220105", 3000);
        if (r) {
            int n = reasm(r, "620105", buf, sizeof(buf));
            if (n) { decode_0105(buf, n); ESP_LOGI(TAG, "0105 raw ok (%d)", n); }
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
done:
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- GATT discovery ---- */
static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902 && !s_cccd_handle) {
        s_cccd_handle = dsc->handle;
        uint8_t en[2] = { 0x01, 0x00 };
        ble_gattc_write_flat(conn, s_cccd_handle, en, 2, NULL, NULL);
        /* subscribed — start the engine */
        set_status("connected");
        if (!s_task) xTaskCreate(obd_task, "obd", 5120, NULL, 4, &s_task);
    }
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (chr) {
        uint8_t pr = chr->properties;
        if ((pr & BLE_GATT_CHR_PROP_NOTIFY) && !s_rx_handle)
            s_rx_handle = chr->val_handle;
        if ((pr & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) && !s_tx_handle) {
            s_tx_handle = chr->val_handle;
            s_tx_norsp  = (pr & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
        }
    } else if (err && err->status == BLE_HS_EDONE) {
        if (s_rx_handle && s_tx_handle) {
            /* find the notify char's CCCD to subscribe */
            ble_gattc_disc_all_dscs(conn, s_rx_handle, 0xFFFF, dsc_cb, NULL);
        } else {
            set_status("no serial characteristics");
            s_state = HC_OBD_ERROR;
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int gap_cb(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s_conn = e->connect.conn_handle;
            s_state = HC_OBD_DISCOVERING; set_status("discovering services");
            s_rx_handle = s_tx_handle = s_cccd_handle = 0;
            ble_gattc_disc_all_chrs(s_conn, 1, 0xFFFF, chr_cb, NULL);
        } else {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_state = HC_OBD_ERROR; set_status("connect failed");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_state = HC_OBD_IDLE; set_status("disconnected");
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (e->notify_rx.conn_handle == s_conn && e->notify_rx.om) {
            uint8_t tmp[64];
            uint16_t got = 0;
            int n = OS_MBUF_PKTLEN(e->notify_rx.om);
            if (n > (int)sizeof(tmp)) n = sizeof(tmp);
            if (ble_hs_mbuf_to_flat(e->notify_rx.om, tmp, n, &got) == 0 && got)
                rx_feed(tmp, got);
        }
        break;
    default: break;
    }
    return 0;
}

void hc_obd_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_resp) s_resp = xSemaphoreCreateBinary();
}

bool hc_obd_connect(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) return false;
    hc_obd_init();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE ||
        s_state == HC_OBD_CONNECTING || s_state == HC_OBD_DISCOVERING) return false;

    hc_ble_stop();                       /* own the radio for the session */
    uint8_t own = 0;
    if (ble_hs_id_infer_auto(0, &own) != 0) return false;

    ble_addr_t peer; peer.type = addr_type; memcpy(peer.val, addr, 6);
    s_state = HC_OBD_CONNECTING; set_status("connecting");
    memset(&s_m, 0, sizeof(s_m));
    if (ble_gap_connect(own, &peer, 10000, NULL, gap_cb, NULL) != 0) {
        s_state = HC_OBD_ERROR; set_status("connect call failed");
        return false;
    }
    return true;
}

void hc_obd_disconnect(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}
