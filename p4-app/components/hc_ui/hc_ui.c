/*
 * Honeycomb — P4 UI (LVGL) — "cyberpunk honeycomb" theme
 *
 * A pannable comb of hexagonal tiles over a warm-charcoal field, with a pinned
 * header (bumblebee mark + status chips). Honey-gold primary, soft cyan accent.
 * Tiles are hexagon OUTLINES drawn with lv_line (stable across LVGL 9.x); the
 * whole comb lives in a scrollable container you can drag in any direction.
 *
 * Orientation: native for now. Landscape rotation lands in a follow-up once the
 * comb is confirmed on-panel (DSI direct-mode rotation is handled separately).
 *
 * Threading: every LVGL touch is wrapped in bsp_display_lock()/unlock().
 */
#include "hc_ui.h"
#include "hc_mode_manager.h"
#include "hc_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if __has_include("bsp/esp-bsp.h")
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#define HAVE_LVGL 1
#else
#define HAVE_LVGL 0
#endif

#define TAG "hc_ui"

/* ---- honeycomb palette ---- */
#define HC_BG       0x15110A   /* warm charcoal field            */
#define HC_HDR      0x15110A   /* header strip                   */
#define HC_PANEL    0x1C160D   /* status chip                    */
#define HC_HONEY    0xFFC233   /* primary                        */
#define HC_AMBER    0xF5A623   /* "waiting" amber                */
#define HC_CYAN     0x38E0E0   /* accent                         */
#define HC_DIMLINE  0x4A3A16   /* empty comb-cell outline        */
#define HC_LHONEY   0xE8C98A   /* honey label                    */
#define HC_LCYAN    0x9EE6E6   /* cyan label                     */
#define HC_DIMTXT   0x8A7A52   /* muted text                     */
#define HC_STRIPE   0x1A1206   /* bee stripes                    */

static bool s_active;

#if HAVE_LVGL

/* Hexagon dimensions (flat-top) and comb spacing. */
#define HEX_W   150
#define HEX_H   130
#define HEX_AX  112            /* column advance = 0.75 * W       */
#define HEX_AY  130            /* row advance    = H              */
#define HDR_H   70
#define COMB_OX 16
#define COMB_OY (HDR_H + 12)
#define COMB_COLS 15
#define COMB_ROWS 8
/* Hidden "buzz" game lives behind this unlabeled cell (pan right to find it). */
#define SECRET_COL 11
#define SECRET_ROW 2

/* One shared outline shape; every cell reuses it (relative points). */
static const lv_point_precise_t HEXP[] = {
    {37, 0}, {113, 0}, {150, 65}, {113, 130}, {37, 130}, {0, 65}, {37, 0}
};

static lv_obj_t *s_mode_lbl;
static lv_obj_t *s_frames_lbl;
static uint32_t  s_frame_count;


enum { TOOL_BENCH, TOOL_EXPLORER, TOOL_CAPTURE, TOOL_DECODE, TOOL_MESH, TOOL_CONFIG };

typedef struct {
    int col, row;
    const char *icon;
    const char *name;
    uint32_t line_color;
    uint32_t text_color;
    int id;
} hc_tile_t;

/* The six live tools, clustered near the comb's center so the hive extends in
 * every direction from the start. Every other cell is an empty comb cell. */
static const hc_tile_t s_tiles[] = {
    {6, 3, LV_SYMBOL_LIST,     "decode",   HC_HONEY, HC_LHONEY, TOOL_DECODE},
    {7, 3, LV_SYMBOL_DOWNLOAD, "bench",    HC_HONEY, HC_LHONEY, TOOL_BENCH},
    {8, 3, LV_SYMBOL_SAVE,     "capture",  HC_HONEY, HC_LHONEY, TOOL_CAPTURE},
    {6, 4, LV_SYMBOL_SETTINGS, "config",   HC_HONEY, HC_LHONEY, TOOL_CONFIG},
    {7, 4, LV_SYMBOL_SHUFFLE,  "mesh",     HC_CYAN,  HC_LCYAN,  TOOL_MESH},
    {8, 4, LV_SYMBOL_WIFI,     "explorer", HC_CYAN,  HC_LCYAN,  TOOL_EXPLORER},
};

static const hc_tile_t *find_tile(int col, int row)
{
    for (size_t i = 0; i < sizeof(s_tiles) / sizeof(s_tiles[0]); i++)
        if (s_tiles[i].col == col && s_tiles[i].row == row) return &s_tiles[i];
    return NULL;
}

static lv_obj_t *bare(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    return o;
}

/* Little bumblebee, built from a few styled objects. */
static void build_bee(lv_obj_t *parent)
{
    lv_obj_t *bee = bare(parent);
    lv_obj_set_size(bee, 34, 30);

    lv_obj_t *wl = lv_obj_create(bee);
    lv_obj_remove_style_all(wl);
    lv_obj_set_size(wl, 15, 11);
    lv_obj_set_pos(wl, 1, 0);
    lv_obj_set_style_radius(wl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wl, lv_color_hex(0xCFEFEF), 0);
    lv_obj_set_style_bg_opa(wl, LV_OPA_60, 0);

    lv_obj_t *wr = lv_obj_create(bee);
    lv_obj_remove_style_all(wr);
    lv_obj_set_size(wr, 15, 11);
    lv_obj_set_pos(wr, 18, 0);
    lv_obj_set_style_radius(wr, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wr, lv_color_hex(0xCFEFEF), 0);
    lv_obj_set_style_bg_opa(wr, LV_OPA_60, 0);

    lv_obj_t *body = lv_obj_create(bee);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 22, 24);
    lv_obj_set_pos(body, 6, 6);
    lv_obj_set_style_radius(body, 11, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(HC_HONEY), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *s = lv_obj_create(bee);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 22, 3);
        lv_obj_set_pos(s, 6, 12 + i * 6);
        lv_obj_set_style_bg_color(s, lv_color_hex(HC_STRIPE), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    }
}

static lv_obj_t *status_chip(lv_obj_t *parent, const char *label, const char *val, uint32_t vcol)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 92, 44);
    lv_obj_set_style_bg_color(chip, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 10, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(0x2A2213), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_pad_all(chip, 4, 0);

    lv_obj_t *l = lv_label_create(chip);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(HC_DIMTXT), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *v = lv_label_create(chip);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_color(v, lv_color_hex(vcol), 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, 0);
    return v;
}

/* ---- tool views: tap a tile -> full-screen overlay with a back button ---- */
static lv_obj_t  *s_overlay;
static bool       s_tool_pending;
static lv_timer_t *s_tool_timer;
static lv_obj_t  *s_dash_psram;
static lv_obj_t  *s_dash_heap;
static lv_obj_t  *s_dash_uptime;
static lv_obj_t  *s_bench_status;
static lv_obj_t  *s_bench_log;
static lv_obj_t  *s_bench_logbox;
static lv_obj_t  *s_bench_bar;
static lv_obj_t  *s_flash_panel;
static char       s_log_buf[2560];
static size_t     s_log_len;
static char       s_flash_path[HC_PATH_MAX];
static char       s_bin_paths[12][HC_PATH_MAX];

static void close_tool(lv_event_t *e)
{
    (void)e;
    hc_flasher_monitor_stop();            /* harmless if no monitor is running */
    if (s_tool_timer) { lv_timer_delete(s_tool_timer); s_tool_timer = NULL; }
    if (s_flash_panel){ lv_obj_delete_async(s_flash_panel); s_flash_panel = NULL; }
    if (s_overlay)    { lv_obj_delete_async(s_overlay); s_overlay = NULL; }
    s_dash_psram = s_dash_heap = s_dash_uptime = NULL;
    s_bench_status = NULL;
    s_bench_log = s_bench_logbox = NULL;
    s_bench_bar = NULL;
    s_baud_row = NULL;
    s_tool_pending = false;
}

static lv_obj_t *tool_shell(const char *title)
{
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(HC_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(s_overlay);
    lv_obj_set_size(back, 120, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_set_style_bg_color(back, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(HC_HONEY), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_add_event_cb(back, close_tool, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  back");
    lv_obj_set_style_text_color(bl, lv_color_hex(HC_HONEY), 0);
    lv_obj_center(bl);

    lv_obj_t *tt = lv_label_create(s_overlay);
    lv_label_set_text(tt, title);
    lv_obj_set_style_text_color(tt, lv_color_hex(HC_HONEY), 0);
    lv_obj_align(tt, LV_ALIGN_TOP_LEFT, 160, 24);
    return s_overlay;
}

static lv_obj_t *dash_row(lv_obj_t *parent, const char *k, const char *v, int y)
{
    lv_obj_t *kl = lv_label_create(parent);
    lv_label_set_text(kl, k);
    lv_obj_set_style_text_color(kl, lv_color_hex(HC_DIMTXT), 0);
    lv_obj_align(kl, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *vl = lv_label_create(parent);
    lv_label_set_text(vl, v);
    lv_obj_set_style_text_color(vl, lv_color_hex(HC_LHONEY), 0);
    lv_obj_align(vl, LV_ALIGN_TOP_LEFT, 200, y);
    return vl;
}

static void dash_timer_cb(lv_timer_t *t)
{
    (void)t;
    char buf[48];
    if (s_dash_psram) {
        snprintf(buf, sizeof(buf), "%u / %u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
        lv_label_set_text(s_dash_psram, buf);
    }
    if (s_dash_heap) {
        snprintf(buf, sizeof(buf), "%u KB free",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        lv_label_set_text(s_dash_heap, buf);
    }
    if (s_dash_uptime) {
        unsigned long s = (unsigned long)(esp_timer_get_time() / 1000000ULL);
        snprintf(buf, sizeof(buf), "%luh %02lum %02lus", s / 3600, (s % 3600) / 60, s % 60);
        lv_label_set_text(s_dash_uptime, buf);
    }
}

static void open_config(void)
{
    lv_obj_t *ov = tool_shell("device");
    lv_obj_t *body = lv_obj_create(ov);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(80), LV_PCT(70));
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 44, 96);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    char buf[48];
    int y = 0;
    const int dy = 42;

    dash_row(body, "CHIP", "ESP32-P4", y); y += dy;
    snprintf(buf, sizeof(buf), "%d", ci.cores);
    dash_row(body, "CORES", buf, y); y += dy;
    snprintf(buf, sizeof(buf), "rev %d", ci.revision);
    dash_row(body, "SILICON", buf, y); y += dy;

    const esp_app_desc_t *ad = esp_app_get_description();
    dash_row(body, "FIRMWARE", ad ? ad->version : "?", y); y += dy;

    s_dash_psram  = dash_row(body, "PSRAM", "...", y); y += dy;
    s_dash_heap   = dash_row(body, "HEAP", "...", y); y += dy;
    s_dash_uptime = dash_row(body, "UPTIME", "...", y); y += dy;

    dash_timer_cb(NULL);
    s_tool_timer = lv_timer_create(dash_timer_cb, 1000, NULL);
}

static void open_msg(const char *title, const char *msg, uint32_t color)
{
    lv_obj_t *ov = tool_shell(title);
    lv_obj_t *m = lv_label_create(ov);
    lv_label_set_text(m, msg);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(m, lv_color_hex(color), 0);
    lv_obj_center(m);
}

/* serial monitor line -> rolling on-screen log */
static void bench_line(const char *line, void *ctx) { (void)ctx; hc_ui_log(line); }

/* ---- baud selector ---- */
static const uint32_t k_bauds[] = { 9600, 115200, 230400, 921600 };
static lv_obj_t *s_baud_row;

static void style_baud_btn(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? HC_HONEY : HC_PANEL), 0);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(active ? HC_BG : HC_LHONEY), 0);
}

static void baud_btn_cb(lv_event_t *e)
{
    uint32_t baud = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    hc_flasher_monitor_start(baud, bench_line, NULL);   /* re-applies line coding live */
    if (s_baud_row) {
        uint32_t n = lv_obj_get_child_count(s_baud_row);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(s_baud_row, i);
            style_baud_btn(c, c == btn);
        }
    }
    char m[24];
    snprintf(m, sizeof(m), "[baud -> %lu]", (unsigned long)baud);
    hc_ui_log(m);
}

/* ---- flash from SD ---- */
static TaskHandle_t s_flash_task;

static void flash_task(void *arg)
{
    (void)arg;
    hc_flasher_monitor_stop();
    vTaskDelay(pdMS_TO_TICKS(200));      /* let the monitor task release the link */
    hc_flash_region_t region = { .offset = 0 };
    strncpy(region.sd_path, s_flash_path, sizeof(region.sd_path) - 1);
    hc_flasher_write(&region, 1);        /* blocks; reports progress via the cb */
    hc_flasher_monitor_start(115200, bench_line, NULL);  /* watch it boot */
    s_flash_task = NULL;
    vTaskDelete(NULL);
}

static void bin_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    strncpy(s_flash_path, s_bin_paths[i], sizeof(s_flash_path) - 1);
    s_flash_path[sizeof(s_flash_path) - 1] = 0;
    if (s_flash_panel) { lv_obj_delete_async(s_flash_panel); s_flash_panel = NULL; }
    if (s_bench_bar) { lv_obj_remove_flag(s_bench_bar, LV_OBJ_FLAG_HIDDEN);
                       lv_bar_set_value(s_bench_bar, 0, LV_ANIM_OFF); }
    if (s_bench_status) lv_label_set_text(s_bench_status, "flashing...");
    if (!s_flash_task)
        xTaskCreate(flash_task, "hc_flash", 6144, NULL, 6, &s_flash_task);
}

static void flash_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (s_flash_panel) { lv_obj_delete_async(s_flash_panel); s_flash_panel = NULL; }
}

/* Built from lv_async (never inside the triggering click) so we don't mutate the
 * LVGL tree mid-event. */
static void async_flash_picker(void *p)
{
    (void)p;
    if (s_flash_panel || s_flash_task) return;

    int n = 0;
    hc_storage_list_bins(s_bin_paths, 12, &n);

    s_flash_panel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_flash_panel, 560, 560);
    lv_obj_center(s_flash_panel);
    lv_obj_set_style_bg_color(s_flash_panel, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_bg_opa(s_flash_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_flash_panel, lv_color_hex(HC_HONEY), 0);
    lv_obj_set_style_border_width(s_flash_panel, 2, 0);
    lv_obj_set_style_radius(s_flash_panel, 12, 0);
    lv_obj_set_flex_flow(s_flash_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_flash_panel, 14, 0);
    lv_obj_set_style_pad_row(s_flash_panel, 8, 0);

    lv_obj_t *t = lv_label_create(s_flash_panel);
    lv_label_set_text(t, "pick a .bin to flash at 0x0");
    lv_obj_set_style_text_color(t, lv_color_hex(HC_HONEY), 0);

    if (n == 0) {
        lv_obj_t *none = lv_label_create(s_flash_panel);
        lv_label_set_text(none, "no .bin files on SD\n(put one in /sdcard or /sdcard/honeycomb)");
        lv_obj_set_style_text_color(none, lv_color_hex(HC_DIMTXT), 0);
    } else {
        for (int i = 0; i < n; i++) {
            lv_obj_t *b = lv_button_create(s_flash_panel);
            lv_obj_set_width(b, LV_PCT(100));
            lv_obj_set_style_bg_color(b, lv_color_hex(HC_STRIPE), 0);
            lv_obj_set_style_border_color(b, lv_color_hex(HC_HONEY), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_t *bl = lv_label_create(b);
            const char *base = strrchr(s_bin_paths[i], '/');
            lv_label_set_text(bl, base ? base + 1 : s_bin_paths[i]);
            lv_obj_set_style_text_color(bl, lv_color_hex(HC_LHONEY), 0);
            lv_obj_add_event_cb(b, bin_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    lv_obj_t *cancel = lv_button_create(s_flash_panel);
    lv_obj_set_width(cancel, LV_PCT(100));
    lv_obj_set_style_bg_color(cancel, lv_color_hex(HC_PANEL), 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(HC_DIMTXT), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, flash_cancel_cb, LV_EVENT_CLICKED, NULL);
}

static void flash_pick_cb(lv_event_t *e)
{
    (void)e;
    if (s_flash_panel || s_flash_task) return;
    lv_async_call(async_flash_picker, NULL);
}

static void open_bench(void)
{
    lv_obj_t *ov = tool_shell("usb bench");

    lv_obj_t *flash_btn = lv_button_create(ov);
    lv_obj_set_size(flash_btn, 190, 48);
    lv_obj_align(flash_btn, LV_ALIGN_TOP_RIGHT, -14, 12);
    lv_obj_set_style_bg_color(flash_btn, lv_color_hex(HC_HONEY), 0);
    lv_obj_set_style_radius(flash_btn, 10, 0);
    lv_obj_t *fbl = lv_label_create(flash_btn);
    lv_label_set_text(fbl, LV_SYMBOL_UPLOAD "  flash from SD");
    lv_obj_set_style_text_color(fbl, lv_color_hex(HC_BG), 0);
    lv_obj_center(fbl);
    lv_obj_add_event_cb(flash_btn, flash_pick_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(ov);
    lv_label_set_text(hint, "USB-A serial monitor");
    lv_obj_set_style_text_color(hint, lv_color_hex(HC_LHONEY), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 44, 92);

    s_bench_status = lv_label_create(ov);
    lv_label_set_long_mode(s_bench_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_bench_status, LV_PCT(84));
    lv_obj_set_style_text_color(s_bench_status, lv_color_hex(HC_CYAN), 0);
    lv_label_set_text(s_bench_status, "starting USB host...");
    lv_obj_align(s_bench_status, LV_ALIGN_TOP_LEFT, 44, 128);

    /* flash progress bar (hidden until a flash starts) */
    s_bench_bar = lv_bar_create(ov);
    lv_obj_set_size(s_bench_bar, LV_PCT(84), 10);
    lv_obj_align(s_bench_bar, LV_ALIGN_TOP_LEFT, 44, 150);
    lv_bar_set_range(s_bench_bar, 0, 100);
    lv_bar_set_value(s_bench_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bench_bar, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_bg_color(s_bench_bar, lv_color_hex(HC_HONEY), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bench_bar, LV_OBJ_FLAG_HIDDEN);

    /* baud selector */
    s_baud_row = lv_obj_create(ov);
    lv_obj_remove_style_all(s_baud_row);
    lv_obj_set_size(s_baud_row, LV_PCT(88), 46);
    lv_obj_align(s_baud_row, LV_ALIGN_TOP_LEFT, 44, 164);
    lv_obj_set_flex_flow(s_baud_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_baud_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_baud_row, 8, 0);
    for (int i = 0; i < (int)(sizeof(k_bauds) / sizeof(k_bauds[0])); i++) {
        lv_obj_t *b = lv_button_create(s_baud_row);
        lv_obj_set_size(b, 118, 40);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(HC_HONEY), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_t *l = lv_label_create(b);
        char t[12];
        snprintf(t, sizeof(t), "%lu", (unsigned long)k_bauds[i]);
        lv_label_set_text(l, t);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, baud_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k_bauds[i]);
        style_baud_btn(b, k_bauds[i] == 115200);
    }

    /* scrolling serial log */
    s_log_buf[0] = 0; s_log_len = 0;
    s_bench_logbox = lv_obj_create(ov);
    lv_obj_remove_style_all(s_bench_logbox);
    lv_obj_set_size(s_bench_logbox, LV_PCT(88), 462);
    lv_obj_align(s_bench_logbox, LV_ALIGN_TOP_LEFT, 44, 220);
    lv_obj_set_style_bg_color(s_bench_logbox, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_bg_opa(s_bench_logbox, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_bench_logbox, 8, 0);
    lv_obj_set_style_radius(s_bench_logbox, 8, 0);
    lv_obj_set_scroll_dir(s_bench_logbox, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_bench_logbox, LV_SCROLLBAR_MODE_AUTO);

    s_bench_log = lv_label_create(s_bench_logbox);
    lv_label_set_long_mode(s_bench_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_bench_log, LV_PCT(100));
    lv_obj_set_style_text_color(s_bench_log, lv_color_hex(HC_LCYAN), 0);
    lv_label_set_text(s_bench_log, "(waiting for serial data)");

    hc_flasher_start();
    hc_flasher_monitor_start(115200, bench_line, NULL);
}

static void async_open_tool(void *p)
{
    s_tool_pending = false;
    if (s_overlay) return;
    int id = (int)(intptr_t)p;
    switch (id) {
        case TOOL_CONFIG: open_config(); break;
        case TOOL_BENCH:  open_bench(); break;
        default:          open_msg("tool", "waiting on the C6 radio\nflash it via the M5 downloader", HC_DIMTXT); break;
    }
}

static void tile_click_cb(lv_event_t *e)
{
    if (s_overlay || s_tool_pending) return;
    s_tool_pending = true;
    lv_async_call(async_open_tool, lv_event_get_user_data(e));
}

/* ---- hidden "buzz" mini-game: flap the bee through the comb ---- */
#define BEE_X   320
#define BEE_SZ  38
#define PIPEW   100
#define GAPH    250
#define GSPEED  7
#define GRAV    1
#define FLAPV   (-15)
#define NPIPE   3
#define GAME_W  1280
#define GAME_H  720

static lv_obj_t *s_bee;
static lv_obj_t *s_ptop[NPIPE], *s_pbot[NPIPE];
static int  s_pipex[NPIPE], s_pipegap[NPIPE];
static bool s_scored[NPIPE];
static int  s_bee_y, s_bee_vy, s_score;
static bool s_game_over;
static lv_obj_t *s_score_lbl, *s_over_lbl;

static void game_place_pipe(int i)
{
    s_pipegap[i] = 150 + (int)(esp_random() % 320);
    lv_obj_set_pos(s_ptop[i], s_pipex[i], 0);
    lv_obj_set_size(s_ptop[i], PIPEW, s_pipegap[i] - GAPH / 2);
    lv_obj_set_pos(s_pbot[i], s_pipex[i], s_pipegap[i] + GAPH / 2);
    lv_obj_set_size(s_pbot[i], PIPEW, GAME_H - (s_pipegap[i] + GAPH / 2));
}

static void game_reset(void)
{
    s_bee_y = GAME_H / 2;
    s_bee_vy = 0;
    s_score = 0;
    s_game_over = false;
    for (int i = 0; i < NPIPE; i++) {
        s_pipex[i] = GAME_W + 120 + i * 440;
        s_scored[i] = false;
        game_place_pipe(i);
    }
    lv_obj_set_pos(s_bee, BEE_X, s_bee_y);
    lv_label_set_text(s_score_lbl, "0");
    lv_obj_add_flag(s_over_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void game_tick(lv_timer_t *t)
{
    (void)t;
    if (s_game_over) return;

    s_bee_vy += GRAV;
    if (s_bee_vy > 22) s_bee_vy = 22;
    s_bee_y += s_bee_vy;
    lv_obj_set_y(s_bee, s_bee_y);

    for (int i = 0; i < NPIPE; i++) {
        s_pipex[i] -= GSPEED;
        if (s_pipex[i] < -PIPEW) {
            int maxx = 0;
            for (int j = 0; j < NPIPE; j++) if (s_pipex[j] > maxx) maxx = s_pipex[j];
            s_pipex[i] = maxx + 440;
            s_scored[i] = false;
            game_place_pipe(i);
        } else {
            lv_obj_set_x(s_ptop[i], s_pipex[i]);
            lv_obj_set_x(s_pbot[i], s_pipex[i]);
        }
        if (!s_scored[i] && s_pipex[i] + PIPEW < BEE_X) {
            s_scored[i] = true;
            s_score++;
            char b[8];
            snprintf(b, sizeof(b), "%d", s_score);
            lv_label_set_text(s_score_lbl, b);
        }
        if (BEE_X + BEE_SZ > s_pipex[i] && BEE_X < s_pipex[i] + PIPEW) {
            if (s_bee_y < s_pipegap[i] - GAPH / 2 || s_bee_y + BEE_SZ > s_pipegap[i] + GAPH / 2)
                s_game_over = true;
        }
    }
    if (s_bee_y < 0 || s_bee_y + BEE_SZ > GAME_H) s_game_over = true;

    if (s_game_over) {
        char b[48];
        snprintf(b, sizeof(b), "game over\nscore %d\ntap to retry", s_score);
        lv_label_set_text(s_over_lbl, b);
        lv_obj_remove_flag(s_over_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void game_flap(lv_event_t *e)
{
    (void)e;
    if (s_game_over) game_reset();
    else s_bee_vy = FLAPV;
}

static void open_game(void)
{
    lv_obj_t *ov = tool_shell("buzz");
    lv_obj_add_event_cb(ov, game_flap, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < NPIPE; i++) {
        s_ptop[i] = lv_obj_create(ov);
        lv_obj_remove_style_all(s_ptop[i]);
        lv_obj_set_style_bg_color(s_ptop[i], lv_color_hex(HC_PANEL), 0);
        lv_obj_set_style_bg_opa(s_ptop[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_ptop[i], lv_color_hex(HC_HONEY), 0);
        lv_obj_set_style_border_width(s_ptop[i], 2, 0);
        s_pbot[i] = lv_obj_create(ov);
        lv_obj_remove_style_all(s_pbot[i]);
        lv_obj_set_style_bg_color(s_pbot[i], lv_color_hex(HC_PANEL), 0);
        lv_obj_set_style_bg_opa(s_pbot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_pbot[i], lv_color_hex(HC_HONEY), 0);
        lv_obj_set_style_border_width(s_pbot[i], 2, 0);
    }

    /* The player is an actual little bee (a container of parts), moved as one
     * by repositioning the container each tick. */
    s_bee = lv_obj_create(ov);
    lv_obj_remove_style_all(s_bee);
    lv_obj_set_size(s_bee, BEE_SZ, BEE_SZ);

    lv_obj_t *wl = lv_obj_create(s_bee);
    lv_obj_remove_style_all(wl);
    lv_obj_set_size(wl, 17, 13);
    lv_obj_set_pos(wl, 2, 1);
    lv_obj_set_style_radius(wl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wl, lv_color_hex(0xCFEFEF), 0);
    lv_obj_set_style_bg_opa(wl, LV_OPA_70, 0);

    lv_obj_t *wr = lv_obj_create(s_bee);
    lv_obj_remove_style_all(wr);
    lv_obj_set_size(wr, 17, 13);
    lv_obj_set_pos(wr, 19, 1);
    lv_obj_set_style_radius(wr, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wr, lv_color_hex(0xCFEFEF), 0);
    lv_obj_set_style_bg_opa(wr, LV_OPA_70, 0);

    lv_obj_t *body = lv_obj_create(s_bee);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 28, 22);
    lv_obj_set_pos(body, 3, 9);
    lv_obj_set_style_radius(body, 11, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(HC_HONEY), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_border_width(body, 2, 0);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *st = lv_obj_create(s_bee);
        lv_obj_remove_style_all(st);
        lv_obj_set_size(st, 28, 3);
        lv_obj_set_pos(st, 3, 15 + i * 6);
        lv_obj_set_style_bg_color(st, lv_color_hex(HC_STRIPE), 0);
        lv_obj_set_style_bg_opa(st, LV_OPA_COVER, 0);
    }

    lv_obj_t *head = lv_obj_create(s_bee);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 10, 10);
    lv_obj_set_pos(head, 28, 15);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);

    s_score_lbl = lv_label_create(ov);
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(HC_HONEY), 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_MID, 0, 22);

    s_over_lbl = lv_label_create(ov);
    lv_obj_set_style_text_align(s_over_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_over_lbl, lv_color_hex(HC_CYAN), 0);
    lv_obj_center(s_over_lbl);

    game_reset();
    s_tool_timer = lv_timer_create(game_tick, 30, NULL);
}

static void async_open_game(void *p)
{
    (void)p;
    s_tool_pending = false;
    if (s_overlay) return;
    open_game();
}

static void game_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_overlay || s_tool_pending) return;
    s_tool_pending = true;
    lv_async_call(async_open_game, NULL);
}

#endif /* HAVE_LVGL */

esp_err_t hc_ui_init(void *lv_display)
{
    if (!lv_display) { ESP_LOGW(TAG, "headless UI"); return ESP_OK; }
#if HAVE_LVGL
    if (!bsp_display_lock(1000)) return ESP_FAIL;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(HC_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* --- pannable comb --- */
    lv_obj_t *cont = bare(scr);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_ALL);
    lv_obj_set_scroll_snap_x(cont, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(cont, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *field = bare(cont);
    lv_obj_set_size(field, COMB_OX * 2 + COMB_COLS * HEX_AX + HEX_W / 4,
                           COMB_OY + COMB_ROWS * HEX_AY + HEX_H);

    /* Pass 1 — decorative cells: just a dim hex outline drawn straight onto the
     * field with no wrapper object. That halves the comb's object count (keeps
     * the LVGL pool from filling up) and, with no rounded caps, keeps the comb
     * repaint cheap when returning from a tool (fast "Back"). These are drawn
     * FIRST so the colored interactive tiles in pass 2 overdraw the shared
     * edges — otherwise a neighbour's dim line paints over a tile's bottom. */
    for (int c = 0; c < COMB_COLS; c++) {
        for (int r = 0; r < COMB_ROWS; r++) {
            if (find_tile(c, r) || (c == SECRET_COL && r == SECRET_ROW)) continue;
            int x = COMB_OX + c * HEX_AX;
            int y = COMB_OY + r * HEX_AY + ((c & 1) ? HEX_H / 2 : 0);
            lv_obj_t *hex = lv_line_create(field);
            lv_obj_set_pos(hex, x, y);
            lv_line_set_points(hex, HEXP, 7);
            lv_obj_set_style_line_color(hex, lv_color_hex(HC_DIMLINE), 0);
            lv_obj_set_style_line_width(hex, 2, 0);
            lv_obj_set_style_line_rounded(hex, false, 0);
        }
    }

    /* Pass 2 — interactive cells on top, so their colored outline wins every
     * shared edge. Each gets a wrapper for its click area and centered labels. */
    for (int c = 0; c < COMB_COLS; c++) {
        for (int r = 0; r < COMB_ROWS; r++) {
            const hc_tile_t *t = find_tile(c, r);
            bool secret = (c == SECRET_COL && r == SECRET_ROW);
            if (!t && !secret) continue;
            int x = COMB_OX + c * HEX_AX;
            int y = COMB_OY + r * HEX_AY + ((c & 1) ? HEX_H / 2 : 0);

            lv_obj_t *cell = bare(field);
            lv_obj_set_pos(cell, x, y);
            lv_obj_set_size(cell, HEX_W, HEX_H);

            lv_obj_t *hex = lv_line_create(cell);
            lv_line_set_points(hex, HEXP, 7);
            lv_obj_set_style_line_color(hex, lv_color_hex(t ? t->line_color : 0x7A6322), 0);
            lv_obj_set_style_line_width(hex, t ? 3 : 2, 0);
            lv_obj_set_style_line_rounded(hex, true, 0);

            if (t) {
                lv_obj_t *ic = lv_label_create(cell);
                lv_label_set_text(ic, t->icon);
                lv_obj_set_style_text_color(ic, lv_color_hex(t->line_color), 0);
                lv_obj_align(ic, LV_ALIGN_CENTER, 0, -14);

                lv_obj_t *nm = lv_label_create(cell);
                lv_label_set_text(nm, t->name);
                lv_obj_set_style_text_color(nm, lv_color_hex(t->text_color), 0);
                lv_obj_align(nm, LV_ALIGN_CENTER, 0, 14);

                /* tappable: a short tap opens the tool, a drag still pans */
                lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(cell, tile_click_cb, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)t->id);
            } else {
                /* hidden game cell: a faint honey tint + dot as the only hint */
                lv_obj_t *dot = lv_label_create(cell);
                lv_label_set_text(dot, ".");
                lv_obj_set_style_text_color(dot, lv_color_hex(HC_HONEY), 0);
                lv_obj_center(dot);
                lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(cell, game_click_cb, LV_EVENT_CLICKED, NULL);
            }
        }
    }

    /* --- pinned header (on top of the comb) --- */
    lv_obj_t *hdr = bare(scr);
    lv_obj_set_size(hdr, LV_PCT(100), HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(HC_HDR), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(hdr, 14, 0);
    lv_obj_set_style_pad_right(hdr, 14, 0);

    build_bee(hdr);
    lv_obj_t *bee_anchor = lv_obj_get_child(hdr, 0);
    lv_obj_align(bee_anchor, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "honeycomb");
    lv_obj_set_style_text_color(title, lv_color_hex(HC_HONEY), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 44, -9);

    lv_obj_t *sub = lv_label_create(hdr);
    lv_label_set_text(sub, "TAB5 \xC2\xB7 802.15.4 EXPLORER");
    lv_obj_set_style_text_color(sub, lv_color_hex(HC_CYAN), 0);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 44, 11);

    lv_obj_t *frames_chip = status_chip(hdr, "FRAMES", "0", HC_CYAN);
    lv_obj_align(lv_obj_get_parent(frames_chip), LV_ALIGN_RIGHT_MID, 0, 0);
    s_frames_lbl = frames_chip;

    lv_obj_t *c6_chip = status_chip(hdr, "C6 RADIO", "waiting", HC_AMBER);
    lv_obj_align(lv_obj_get_parent(c6_chip), LV_ALIGN_RIGHT_MID, -100, 0);
    s_mode_lbl = c6_chip;

    /* Start centered so the comb extends in every direction from the first view. */
    lv_obj_update_layout(scr);
    int vw = lv_obj_get_width(cont),  vh = lv_obj_get_height(cont);
    int fw = lv_obj_get_width(field), fh = lv_obj_get_height(field);
    int sx = (fw - vw) / 2, sy = (fh - vh) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    lv_obj_scroll_to(cont, sx, sy, LV_ANIM_OFF);

    bsp_display_unlock();
    s_active = true;
    ESP_LOGI(TAG, "honeycomb UI up");
#endif
    return ESP_OK;
}

void hc_ui_add_frame(const hc_dissected_t *row)
{
#if HAVE_LVGL
    if (!s_active || !row) return;
    s_frame_count++;
    if (bsp_display_lock(50)) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)s_frame_count);
        lv_label_set_text(s_frames_lbl, buf);
        bsp_display_unlock();
    }
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
    const char *n = mode == HC_MODE_SNIFFER ? "sniffer" :
                    mode == HC_MODE_WIFI ? "wifi" : "idle";
    if (bsp_display_lock(50)) {
        lv_label_set_text(s_mode_lbl, ok ? n : "fail");
        lv_obj_set_style_text_color(s_mode_lbl, lv_color_hex(ok ? HC_HONEY : HC_AMBER), 0);
        bsp_display_unlock();
    }
#else
    (void)mode; (void)ok;
#endif
}

void hc_ui_set_flash_progress(const hc_flash_progress_t *p)
{
#if HAVE_LVGL
    if (!p) return;
    if (bsp_display_lock(50)) {
        if (s_bench_status) {
            if (p->bytes_total > 0) {
                int pct = (int)((uint64_t)p->bytes_done * 100 / p->bytes_total);
                char b[96];
                snprintf(b, sizeof(b), "%s  %d%%", p->message, pct);
                lv_label_set_text(s_bench_status, b);
                if (s_bench_bar) {
                    lv_obj_remove_flag(s_bench_bar, LV_OBJ_FLAG_HIDDEN);
                    lv_bar_set_value(s_bench_bar, pct, LV_ANIM_OFF);
                }
            } else {
                lv_label_set_text(s_bench_status, p->message);
            }
        }
        bsp_display_unlock();
    }
#else
    (void)p;
#endif
}

void hc_ui_log(const char *line)
{
#if HAVE_LVGL
    if (!line) return;
    if (bsp_display_lock(50)) {
        if (s_bench_log) {
            size_t ll = strlen(line);
            if (ll > sizeof(s_log_buf) - 2) { line += ll - (sizeof(s_log_buf) - 2); ll = strlen(line); }
            /* make room by dropping whole lines from the front */
            while (s_log_len + ll + 2 >= sizeof(s_log_buf) && s_log_len) {
                char *nl = memchr(s_log_buf, '\n', s_log_len);
                size_t drop = nl ? (size_t)(nl - s_log_buf) + 1 : s_log_len;
                memmove(s_log_buf, s_log_buf + drop, s_log_len - drop);
                s_log_len -= drop;
            }
            memcpy(s_log_buf + s_log_len, line, ll); s_log_len += ll;
            s_log_buf[s_log_len++] = '\n';
            s_log_buf[s_log_len] = 0;
            lv_label_set_text(s_bench_log, s_log_buf);
            if (s_bench_logbox) lv_obj_scroll_to_y(s_bench_logbox, LV_COORD_MAX / 2, LV_ANIM_OFF);
        }
        bsp_display_unlock();
    }
#else
    (void)line;
#endif
}
