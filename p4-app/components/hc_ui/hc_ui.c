/*
 * Honeycomb — P4 UI implementation (LVGL)
 *
 * This builds the widget tree and wires buttons to the control modules. It is
 * intentionally compact; styling/polish is a later pass. When lv_display is NULL
 * (display BSP not yet bound) every entry point becomes a no-op so the rest of
 * the firmware runs headless and testable.
 */
#include "hc_ui.h"
#include "hc_mode_manager.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#define TAG "hc_ui"

#if __has_include("lvgl.h")
#include "lvgl.h"
#define HAVE_LVGL 1
#else
#define HAVE_LVGL 0
#endif

static bool s_active;

#if HAVE_LVGL
static lv_obj_t *s_tabview;
static lv_obj_t *s_frame_table;   /* explorer capture list */
static lv_obj_t *s_mode_badge;
static lv_obj_t *s_flash_bar;
static lv_obj_t *s_flash_label;
static lv_obj_t *s_log;
static uint16_t  s_rows;

/* ---- EXPLORER tab controls ---- */
static void ev_start_sniff(lv_event_t *e)
{
    hc_mode_manager_set_mode(HC_MODE_SNIFFER);
    hc_sniff_cfg_t cfg = { .channel = 15, .promiscuous = 1 };
    hc_mode_manager_sniff_start(&cfg);
}
static void ev_stop_sniff(lv_event_t *e)   { hc_mode_manager_sniff_stop(); }
static void ev_energy_scan(lv_event_t *e)  { hc_mode_manager_energy_scan(0xFFFF, 120); }
static void ev_chan_roller(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_target(e);
    uint8_t ch = 11 + lv_roller_get_selected(r);
    hc_mode_manager_set_channel(ch);
}

/* ---- BENCH tab controls ---- */
static void ev_flash(lv_event_t *e)
{
    /* Example: flash a merged image sitting on SD. The file browser is a later
     * pass; for now it points at a known path. */
    hc_flash_region_t r = { .offset = 0x0 };
    strncpy(r.sd_path, "/sdcard/honeycomb/firmware.bin", sizeof(r.sd_path));
    hc_flasher_write(&r, 1);
}

static void build_explorer_tab(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), 54);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    lv_obj_t *b1 = lv_btn_create(bar);
    lv_obj_add_event_cb(b1, ev_start_sniff, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(b1), "Sniff");
    lv_obj_t *b2 = lv_btn_create(bar);
    lv_obj_add_event_cb(b2, ev_stop_sniff, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(b2), "Stop");
    lv_obj_t *b3 = lv_btn_create(bar);
    lv_obj_add_event_cb(b3, ev_energy_scan, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(b3), "Energy");

    lv_obj_t *roller = lv_roller_create(bar);
    lv_roller_set_options(roller,
        "11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26",
        LV_ROLLER_MODE_NORMAL);
    lv_obj_add_event_cb(roller, ev_chan_roller, LV_EVENT_VALUE_CHANGED, NULL);

    s_frame_table = lv_table_create(parent);
    lv_obj_set_size(s_frame_table, lv_pct(100), lv_pct(80));
    lv_table_set_col_cnt(s_frame_table, 5);
    lv_table_set_row_cnt(s_frame_table, 1);
    lv_table_set_cell_value(s_frame_table, 0, 0, "Ch");
    lv_table_set_cell_value(s_frame_table, 0, 1, "Type");
    lv_table_set_cell_value(s_frame_table, 0, 2, "Src");
    lv_table_set_cell_value(s_frame_table, 0, 3, "Dst");
    lv_table_set_cell_value(s_frame_table, 0, 4, "RSSI");
    s_rows = 1;
}

static void build_bench_tab(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_event_cb(btn, ev_flash, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn), "Flash /sdcard/honeycomb/firmware.bin");

    s_flash_bar = lv_bar_create(parent);
    lv_obj_set_size(s_flash_bar, lv_pct(90), 18);
    lv_bar_set_range(s_flash_bar, 0, 100);

    s_flash_label = lv_label_create(parent);
    lv_label_set_text(s_flash_label, "idle");

    s_log = lv_textarea_create(parent);
    lv_obj_set_size(s_log, lv_pct(100), lv_pct(55));
    lv_textarea_set_text(s_log, "");
}

static void build_status_tab(lv_obj_t *parent)
{
    s_mode_badge = lv_label_create(parent);
    lv_label_set_text(s_mode_badge, "C6: connecting…");
}
#endif /* HAVE_LVGL */

esp_err_t hc_ui_init(void *lv_display)
{
    if (!lv_display) { ESP_LOGW(TAG, "headless UI"); return ESP_OK; }
#if HAVE_LVGL
    s_tabview = lv_tabview_create(lv_scr_act());
    build_bench_tab(lv_tabview_add_tab(s_tabview, "Bench"));
    build_explorer_tab(lv_tabview_add_tab(s_tabview, "Explorer"));
    build_status_tab(lv_tabview_add_tab(s_tabview, "Status"));
    s_active = true;
#endif
    return ESP_OK;
}

void hc_ui_add_frame(const hc_dissected_t *row)
{
#if HAVE_LVGL
    if (!s_active || !row) return;
    char ch[4], rssi[6];
    snprintf(ch, sizeof(ch), "%u", row->channel);
    snprintf(rssi, sizeof(rssi), "%d", row->rssi);
    uint16_t r = s_rows++;
    if (r > 200) { s_rows = 1; r = 1; } /* ring the table to bound memory */
    lv_table_set_row_cnt(s_frame_table, s_rows);
    lv_table_set_cell_value(s_frame_table, r, 0, ch);
    lv_table_set_cell_value(s_frame_table, r, 1, row->type_str ? row->type_str : "?");
    lv_table_set_cell_value(s_frame_table, r, 2, row->src[0] ? row->src : "-");
    lv_table_set_cell_value(s_frame_table, r, 3, row->dst[0] ? row->dst : "-");
    lv_table_set_cell_value(s_frame_table, r, 4, rssi);
#else
    (void)row;
#endif
}

void hc_ui_set_energy(const hc_energy_result_t *e) { (void)e; }
void hc_ui_add_scan(const hc_scan_result_t *s)     { (void)s; }

void hc_ui_set_mode(hc_radio_mode_t mode, bool ok)
{
#if HAVE_LVGL
    if (!s_active) return;
    const char *n = mode == HC_MODE_SNIFFER ? "SNIFFER" :
                    mode == HC_MODE_WIFI ? "WIFI" : "IDLE";
    char buf[48]; snprintf(buf, sizeof(buf), "C6: %s %s", n, ok ? "ok" : "FAIL");
    lv_label_set_text(s_mode_badge, buf);
#else
    (void)mode; (void)ok;
#endif
}

void hc_ui_set_flash_progress(const hc_flash_progress_t *p)
{
#if HAVE_LVGL
    if (!s_active || !p) return;
    int pct = p->bytes_total ? (int)(100ull * p->bytes_done / p->bytes_total) : 0;
    lv_bar_set_value(s_flash_bar, pct, LV_ANIM_OFF);
    char buf[80]; snprintf(buf, sizeof(buf), "%s [%s] %d%%",
                           p->message, hc_target_chip_name(p->chip), pct);
    lv_label_set_text(s_flash_label, buf);
#else
    (void)p;
#endif
}

void hc_ui_log(const char *line)
{
#if HAVE_LVGL
    if (s_active && s_log && line) { lv_textarea_add_text(s_log, line); lv_textarea_add_char(s_log, '\n'); }
#else
    (void)line;
#endif
}
