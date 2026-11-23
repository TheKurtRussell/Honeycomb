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
#include "hc_c6ota.h"
#include "hc_ble.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_netif.h"
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
#define HC_GREEN    0x3BD16F   /* indicator: active              */
#define HC_RED      0xE0533B   /* indicator: off                 */

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

/* One shared outline shape; every cell reuses it (relative points). */
static const lv_point_precise_t HEXP[] = {
    {37, 0}, {113, 0}, {150, 65}, {113, 130}, {37, 130}, {0, 65}, {37, 0}
};

static lv_obj_t *s_wifi_dot;
static lv_obj_t *s_ble_dot;
static lv_obj_t *s_frames_lbl;
static uint32_t  s_frame_count;


enum { TOOL_BENCH, TOOL_EXPLORER, TOOL_BLE, TOOL_DECODE, TOOL_MESH, TOOL_CONFIG, TOOL_FILES };

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
    {7, 2, LV_SYMBOL_DIRECTORY, "files",   HC_HONEY, HC_LHONEY, TOOL_FILES},
    {6, 3, LV_SYMBOL_LIST,     "decode",   HC_HONEY, HC_LHONEY, TOOL_DECODE},
    {7, 3, LV_SYMBOL_DOWNLOAD, "bench",    HC_HONEY, HC_LHONEY, TOOL_BENCH},
    {8, 3, LV_SYMBOL_BLUETOOTH, "ble",     HC_CYAN,  HC_LCYAN,  TOOL_BLE},
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

/* A labelled status dot: "WIFI ●". Returns the dot so its colour can change. */
static lv_obj_t *radio_dot(lv_obj_t *parent, const char *label)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 86, 44);
    lv_obj_set_style_bg_color(chip, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 10, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(0x2A2213), 0);
    lv_obj_set_style_border_width(chip, 1, 0);

    lv_obj_t *l = lv_label_create(chip);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(HC_DIMTXT), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *dot = lv_obj_create(chip);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(HC_RED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -10, 0);
    return dot;
}

/* ---- tool views: tap a tile -> full-screen overlay with a back button ---- */
static lv_obj_t  *s_overlay;
static lv_obj_t  *s_comb;        /* the pannable comb container (hidden for tools, freed for the game) */
static bool       s_comb_hidden; /* true when the comb is kept alive but hidden behind an overlay */
static lv_obj_t  *s_scr;         /* active screen, for rebuilding the comb */
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
static lv_obj_t  *s_baud_row;
static lv_obj_t  *s_fb_list;
static lv_obj_t  *s_fb_pathlbl;
static lv_obj_t  *s_wifi_list;        /* explorer: AP list container */
static lv_obj_t  *s_wifi_status;      /* explorer: "scanning..."/"N networks" */
static lv_obj_t  *s_ble_list;         /* ble: device list container */
static lv_obj_t  *s_ble_status;       /* ble: "scanning..."/"N devices" */
static wifi_ap_record_t s_wifi_recs[30];
static uint16_t   s_wifi_n;
static volatile bool s_wifi_scanning;
static lv_obj_t  *s_conn_panel;       /* explorer: connect dialog */
static lv_obj_t  *s_conn_pw;          /* explorer: password textarea */
static lv_obj_t  *s_conn_kb;          /* explorer: on-screen keyboard */
static char       s_sel_ssid[33];     /* AP chosen to connect to */
static char       s_conn_result[72];  /* connect outcome, applied on LVGL thread */
static volatile bool s_connecting;
static lv_obj_t  *s_c6ota_panel;      /* config: C6-update confirm/progress panel */
static lv_obj_t  *s_c6ota_status;     /* config: C6-update status label */
static lv_obj_t  *s_c6ota_bar;        /* config: C6-update progress bar */
static volatile bool s_c6ota_running;
static int        s_c6ota_pct, s_c6ota_last;
static char       s_c6ota_msg[40];
static char       s_fb_path[160];
static char       s_log_buf[2560];
static size_t     s_log_len;
static char       s_flash_path[HC_PATH_MAX];
static char       s_bin_paths[12][HC_PATH_MAX];
static uint32_t   s_cur_baud = 115200;

static void tile_click_cb(lv_event_t *e);

/* Build (or rebuild) the pannable comb as a child of the screen. Kept in one
 * place so it can be torn down while a tool overlay is open (freeing the LVGL
 * pool for that overlay) and recreated on close. Caller holds the display lock. */
static void build_comb(void)
{
    if (s_comb) return;
    s_scr = lv_screen_active();

    lv_obj_t *cont = bare(s_scr);
    s_comb = cont;
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    /* Static splash: no panning. The comb is laid out once and the functional
     * node cluster is centered in the area below the header (see offset below). */
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *field = bare(cont);
    lv_obj_set_size(field, COMB_OX * 2 + COMB_COLS * HEX_AX + HEX_W / 4,
                           COMB_OY + COMB_ROWS * HEX_AY + HEX_H);

    /* Track the bounding box of the interactive tiles so we can center them. */
    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;

    /* Pass 1 — decorative cells (dim hex outline, no wrapper, square caps). */
    for (int c = 0; c < COMB_COLS; c++) {
        for (int r = 0; r < COMB_ROWS; r++) {
            if (find_tile(c, r)) continue;
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

    /* Pass 2 — interactive tiles on top, so their colored outline wins. */
    for (int c = 0; c < COMB_COLS; c++) {
        for (int r = 0; r < COMB_ROWS; r++) {
            const hc_tile_t *t = find_tile(c, r);
            if (!t) continue;
            int x = COMB_OX + c * HEX_AX;
            int y = COMB_OY + r * HEX_AY + ((c & 1) ? HEX_H / 2 : 0);

            if (x < minx) minx = x;
            if (y < miny) miny = y;
            if (x + HEX_W > maxx) maxx = x + HEX_W;
            if (y + HEX_H > maxy) maxy = y + HEX_H;

            lv_obj_t *cell = bare(field);
            lv_obj_set_pos(cell, x, y);
            lv_obj_set_size(cell, HEX_W, HEX_H);

            lv_obj_t *hex = lv_line_create(cell);
            lv_line_set_points(hex, HEXP, 7);
            lv_obj_set_style_line_color(hex, lv_color_hex(t->line_color), 0);
            lv_obj_set_style_line_width(hex, 3, 0);
            lv_obj_set_style_line_rounded(hex, true, 0);

            lv_obj_t *ic = lv_label_create(cell);
            lv_label_set_text(ic, t->icon);
            lv_obj_set_style_text_color(ic, lv_color_hex(t->line_color), 0);
            lv_obj_align(ic, LV_ALIGN_CENTER, 0, -14);

            lv_obj_t *nm = lv_label_create(cell);
            lv_label_set_text(nm, t->name);
            lv_obj_set_style_text_color(nm, lv_color_hex(t->text_color), 0);
            lv_obj_align(nm, LV_ALIGN_CENTER, 0, 14);

            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, tile_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)t->id);
        }
    }

    lv_obj_move_background(cont);   /* keep the comb below the pinned header */

    /* Shift the field so the interactive-node cluster is centered in the area
     * below the header. The decorative comb fills the rest and clips at the
     * screen edges — a static splash, no scrolling needed. */
    lv_obj_update_layout(s_scr);
    int vw = lv_obj_get_width(cont), vh = lv_obj_get_height(cont);
    if (minx <= maxx && miny <= maxy) {
        int bbw = maxx - minx, bbh = maxy - miny;
        int offx = (vw - bbw) / 2 - minx;
        int offy = HDR_H + (vh - HDR_H - bbh) / 2 - miny;
        lv_obj_set_pos(field, offx, offy);
    }
}

static void rebuild_comb_async(void *p) { (void)p; build_comb(); }

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
    s_fb_list = s_fb_pathlbl = NULL;
    s_wifi_list = s_wifi_status = NULL;
    s_ble_list = s_ble_status = NULL;
    s_conn_panel = s_conn_pw = s_conn_kb = NULL;
    s_c6ota_panel = s_c6ota_status = s_c6ota_bar = NULL;
    s_tool_pending = false;
    /* Bring the comb back. If it was only hidden (a tool), just un-hide it —
     * instant, no rebuild. If it was freed (the game), rebuild it async. */
    if (s_comb && s_comb_hidden) {
        lv_obj_remove_flag(s_comb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_background(s_comb);
        s_comb_hidden = false;
    } else {
        lv_async_call(rebuild_comb_async, NULL);
    }
}

static lv_obj_t *tool_shell_ex(const char *title, bool free_comb)
{
    /* The comb sits behind the overlay. For tools we HIDE it (kept in the pool,
     * so reopening/closing is instant). For the memory-heavy game we DELETE it
     * so the whole LVGL pool is free. tool_shell_ex is only reached from
     * lv_async (not inside an event), so deleting here is safe. */
    if (free_comb) {
        if (s_comb) { lv_obj_delete(s_comb); s_comb = NULL; }
        s_comb_hidden = false;
    } else if (s_comb) {
        lv_obj_add_flag(s_comb, LV_OBJ_FLAG_HIDDEN);
        s_comb_hidden = true;
    }

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

/* Tools keep the comb alive (hidden) for instant open/close. */
static lv_obj_t *tool_shell(const char *title) { return tool_shell_ex(title, false); }

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

/* ---- C6 radio update (host-driven OTA to the BLE-capable slave) ---- */

/* Apply progress to the UI (LVGL thread, via lv_async). */
static void c6ota_ui_apply(void *p)
{
    (void)p;
    if (s_c6ota_bar)    lv_bar_set_value(s_c6ota_bar, s_c6ota_pct, LV_ANIM_OFF);
    if (s_c6ota_status) lv_label_set_text(s_c6ota_status, s_c6ota_msg);
}

/* Progress callback from the OTA worker; throttle UI updates to pct changes. */
static void c6ota_cb(int pct, const char *msg, void *ctx)
{
    (void)ctx;
    s_c6ota_pct = pct;
    strlcpy(s_c6ota_msg, msg, sizeof(s_c6ota_msg));
    if (pct != s_c6ota_last) { s_c6ota_last = pct; lv_async_call(c6ota_ui_apply, NULL); }
}

static void c6ota_task(void *arg)
{
    (void)arg;
    esp_err_t r = hc_c6ota_from_partition("c6fw", c6ota_cb, NULL);
    if (r == ESP_OK) {
        strlcpy(s_c6ota_msg, "activating — C6 reboot", sizeof(s_c6ota_msg));
        lv_async_call(c6ota_ui_apply, NULL);
        vTaskDelay(pdMS_TO_TICKS(500));
        hc_c6ota_activate_and_restart();   /* reboots the P4 — no return */
    }
    s_c6ota_pct = 0;
    strlcpy(s_c6ota_msg, "update failed — C6 unchanged", sizeof(s_c6ota_msg));
    s_c6ota_running = false;
    lv_async_call(c6ota_ui_apply, NULL);
    vTaskDelete(NULL);
}

/* LVGL thread (async, not inside an event): swap the confirm panel for a
 * progress view and kick off the OTA worker. */
static void c6ota_begin(void *p)
{
    (void)p;
    if (s_c6ota_running || !s_c6ota_panel) return;
    lv_obj_clean(s_c6ota_panel);
    s_c6ota_status = s_c6ota_bar = NULL;

    lv_obj_t *t = lv_label_create(s_c6ota_panel);
    lv_label_set_text(t, "Updating C6 radio — do not power off");
    lv_obj_set_style_text_color(t, lv_color_hex(HC_HONEY), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 6, 6);

    s_c6ota_bar = lv_bar_create(s_c6ota_panel);
    lv_obj_set_size(s_c6ota_bar, LV_PCT(92), 18);
    lv_obj_align(s_c6ota_bar, LV_ALIGN_TOP_MID, 0, 44);
    lv_bar_set_range(s_c6ota_bar, 0, 100);
    lv_bar_set_value(s_c6ota_bar, 0, LV_ANIM_OFF);

    s_c6ota_status = lv_label_create(s_c6ota_panel);
    lv_label_set_text(s_c6ota_status, "starting...");
    lv_obj_set_style_text_color(s_c6ota_status, lv_color_hex(HC_CYAN), 0);
    lv_obj_align(s_c6ota_status, LV_ALIGN_TOP_MID, 0, 74);

    s_c6ota_last = -1;
    s_c6ota_running = true;
    if (xTaskCreate(c6ota_task, "c6ota", 6144, NULL, 4, NULL) != pdPASS) {
        s_c6ota_running = false;
        lv_label_set_text(s_c6ota_status, "could not start");
    }
}

static void c6ota_go_cb(lv_event_t *e)    { (void)e; lv_async_call(c6ota_begin, NULL); }
static void c6ota_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (s_c6ota_panel) { lv_obj_delete_async(s_c6ota_panel); s_c6ota_panel = NULL; }
    s_c6ota_status = s_c6ota_bar = NULL;
}

/* "Update C6 radio" button handler -> confirmation panel. */
static void c6ota_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_c6ota_panel || s_c6ota_running) return;

    s_c6ota_panel = lv_obj_create(s_overlay);
    lv_obj_set_size(s_c6ota_panel, 600, 240);
    lv_obj_align(s_c6ota_panel, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_c6ota_panel, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(s_c6ota_panel, lv_color_hex(HC_HONEY), 0);
    lv_obj_set_style_border_width(s_c6ota_panel, 1, 0);
    lv_obj_set_style_radius(s_c6ota_panel, 10, 0);
    lv_obj_remove_flag(s_c6ota_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *w = lv_label_create(s_c6ota_panel);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, LV_PCT(94));
    lv_label_set_text(w,
        "Reflash the C6 radio to the BLE-capable firmware?\n"
        "Takes ~30-60s over SDIO. If it fails, the C6 falls back to the factory "
        "firmware on its own. The device reboots when done.");
    lv_obj_set_style_text_color(w, lv_color_hex(HC_LHONEY), 0);
    lv_obj_align(w, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_obj_t *go = lv_button_create(s_c6ota_panel);
    lv_obj_set_size(go, 180, 54);
    lv_obj_align(go, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_style_bg_color(go, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_border_color(go, lv_color_hex(HC_GREEN), 0);
    lv_obj_set_style_border_width(go, 1, 0);
    lv_obj_add_event_cb(go, c6ota_go_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(go); lv_label_set_text(gl, "update C6");
    lv_obj_set_style_text_color(gl, lv_color_hex(HC_GREEN), 0); lv_obj_center(gl);

    lv_obj_t *cx = lv_button_create(s_c6ota_panel);
    lv_obj_set_size(cx, 180, 54);
    lv_obj_align(cx, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_style_bg_color(cx, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_border_color(cx, lv_color_hex(HC_DIMTXT), 0);
    lv_obj_set_style_border_width(cx, 1, 0);
    lv_obj_add_event_cb(cx, c6ota_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cx); lv_label_set_text(cl, "cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(HC_DIMTXT), 0); lv_obj_center(cl);
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

    /* Update the on-board C6 radio to the BLE-capable slave (host-driven OTA). */
    lv_obj_t *ota = lv_button_create(ov);
    lv_obj_set_size(ota, 300, 60);
    lv_obj_align(ota, LV_ALIGN_BOTTOM_LEFT, 44, -40);
    lv_obj_set_style_bg_color(ota, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(ota, lv_color_hex(HC_HONEY), 0);
    lv_obj_set_style_border_width(ota, 1, 0);
    lv_obj_add_event_cb(ota, c6ota_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(ota);
    lv_label_set_text(ol, LV_SYMBOL_REFRESH "  Update C6 radio (BLE)");
    lv_obj_set_style_text_color(ol, lv_color_hex(HC_HONEY), 0);
    lv_obj_center(ol);

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
    s_cur_baud = baud;
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

    /* Don't flash until a serial device is fully open — flashing into a
     * half-open CDC handle faults. Wait briefly in case the open is in flight. */
    int waited = 0;
    while (!hc_flasher_is_ready() && waited < 3000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }
    if (!hc_flasher_is_ready()) {
        hc_ui_log("no serial device open — plug a board in first");
        hc_flasher_monitor_start(s_cur_baud, bench_line, NULL);
        s_flash_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    hc_flash_region_t region = { .offset = 0 };
    strncpy(region.sd_path, s_flash_path, sizeof(region.sd_path) - 1);
    hc_flasher_write(&region, 1);        /* blocks; reports progress via the cb */
    hc_flasher_monitor_start(s_cur_baud, bench_line, NULL);  /* watch it boot */
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

    if (!s_overlay) return;               /* bench closed before async ran */

    int n = 0;
    hc_storage_list_bins(s_bin_paths, 12, &n);

    /* Child of the bench overlay (same as every other panel) rather than a
     * sibling on the scrollable screen. */
    s_flash_panel = lv_obj_create(s_overlay);
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

static void clear_log_cb(lv_event_t *e)
{
    (void)e;
    s_log_buf[0] = 0; s_log_len = 0;
    if (s_bench_log) lv_label_set_text(s_bench_log, "(cleared)");
}

static void save_log_cb(lv_event_t *e)
{
    (void)e;
    char path[HC_PATH_MAX];
    esp_err_t r = hc_storage_save_log(s_log_buf, path, sizeof(path));
    if (s_bench_status) {
        if (r == ESP_OK) {
            char m[160];
            snprintf(m, sizeof(m), "saved log to %s", path);
            lv_label_set_text(s_bench_status, m);
        } else {
            lv_label_set_text(s_bench_status, "could not save log (no SD?)");
        }
    }
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

    lv_obj_t *save_btn = lv_button_create(ov);
    lv_obj_set_size(save_btn, 90, 40);
    lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, -214, 16);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(save_btn, lv_color_hex(HC_CYAN), 0);
    lv_obj_set_style_border_width(save_btn, 1, 0);
    lv_obj_set_style_radius(save_btn, 10, 0);
    lv_obj_t *sbl = lv_label_create(save_btn);
    lv_label_set_text(sbl, "save");
    lv_obj_set_style_text_color(sbl, lv_color_hex(HC_LCYAN), 0);
    lv_obj_center(sbl);
    lv_obj_add_event_cb(save_btn, save_log_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear_btn = lv_button_create(ov);
    lv_obj_set_size(clear_btn, 90, 40);
    lv_obj_align(clear_btn, LV_ALIGN_TOP_RIGHT, -314, 16);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(clear_btn, lv_color_hex(HC_CYAN), 0);
    lv_obj_set_style_border_width(clear_btn, 1, 0);
    lv_obj_set_style_radius(clear_btn, 10, 0);
    lv_obj_t *cbl = lv_label_create(clear_btn);
    lv_label_set_text(cbl, "clear");
    lv_obj_set_style_text_color(cbl, lv_color_hex(HC_LCYAN), 0);
    lv_obj_center(cbl);
    lv_obj_add_event_cb(clear_btn, clear_log_cb, LV_EVENT_CLICKED, NULL);

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
        style_baud_btn(b, k_bauds[i] == s_cur_baud);
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
    hc_flasher_monitor_start(s_cur_baud, bench_line, NULL);
}

/* ---- SD file browser ---- */
static hc_dirent_t s_fb_entries[64];
static void fb_populate(void);
static void fb_refresh(void *p) { (void)p; fb_populate(); }

static void fb_up_cb(lv_event_t *e)
{
    (void)e;
    char *slash = strrchr(s_fb_path, '/');
    if (slash && slash != s_fb_path) *slash = 0;
    if (strlen(s_fb_path) < 7) strcpy(s_fb_path, "/sdcard");   /* floor at mount */
    lv_async_call(fb_refresh, NULL);     /* rebuild outside the click event */
}

static void fb_dir_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    size_t l = strlen(s_fb_path);
    snprintf(s_fb_path + l, sizeof(s_fb_path) - l, "/%s", name);
    lv_async_call(fb_refresh, NULL);
}

static lv_obj_t *fb_row(const char *text, bool tappable)
{
    lv_obj_t *b = lv_button_create(s_fb_list);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_bg_color(b, lv_color_hex(tappable ? HC_STRIPE : HC_PANEL), 0);
    lv_obj_set_style_radius(b, 6, 0);
    if (!tappable) lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(tappable ? HC_LHONEY : HC_LCYAN), 0);
    return b;
}

static void fb_populate(void)
{
    if (!s_fb_list) return;
    lv_obj_clean(s_fb_list);                 /* drop previous rows */
    if (s_fb_pathlbl) lv_label_set_text(s_fb_pathlbl, s_fb_path);

    int n = 0;
    hc_storage_list_dir(s_fb_path, s_fb_entries, 64, &n);

    if (strcmp(s_fb_path, "/sdcard") != 0) {
        lv_obj_t *up = fb_row(LV_SYMBOL_UP "  ..", true);
        lv_obj_add_event_cb(up, fb_up_cb, LV_EVENT_CLICKED, NULL);
    }
    for (int i = 0; i < n; i++) {
        char label[112];
        if (s_fb_entries[i].is_dir)
            snprintf(label, sizeof(label), LV_SYMBOL_DIRECTORY "  %s", s_fb_entries[i].name);
        else
            snprintf(label, sizeof(label), LV_SYMBOL_FILE "  %s   (%lu B)",
                     s_fb_entries[i].name, (unsigned long)s_fb_entries[i].size);
        lv_obj_t *row = fb_row(label, s_fb_entries[i].is_dir);
        if (s_fb_entries[i].is_dir)
            lv_obj_add_event_cb(row, fb_dir_cb, LV_EVENT_CLICKED, s_fb_entries[i].name);
    }
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_fb_list);
        lv_label_set_text(empty, "(empty)");
        lv_obj_set_style_text_color(empty, lv_color_hex(HC_DIMTXT), 0);
    }
}

static void open_files(void)
{
    lv_obj_t *ov = tool_shell("files");
    strcpy(s_fb_path, "/sdcard");

    s_fb_pathlbl = lv_label_create(ov);
    lv_label_set_long_mode(s_fb_pathlbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_fb_pathlbl, LV_PCT(84));
    lv_obj_set_style_text_color(s_fb_pathlbl, lv_color_hex(HC_CYAN), 0);
    lv_obj_align(s_fb_pathlbl, LV_ALIGN_TOP_LEFT, 44, 98);
    lv_label_set_text(s_fb_pathlbl, s_fb_path);

    s_fb_list = lv_obj_create(ov);
    lv_obj_remove_style_all(s_fb_list);
    lv_obj_set_size(s_fb_list, LV_PCT(88), 520);
    lv_obj_align(s_fb_list, LV_ALIGN_TOP_LEFT, 44, 134);
    lv_obj_set_style_bg_color(s_fb_list, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_bg_opa(s_fb_list, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(s_fb_list, 8, 0);
    lv_obj_set_style_radius(s_fb_list, 8, 0);
    lv_obj_set_flex_flow(s_fb_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_fb_list, 6, 0);
    lv_obj_set_scroll_dir(s_fb_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_fb_list, LV_SCROLLBAR_MODE_AUTO);

    fb_populate();
}

/* ---- explorer: live Wi-Fi scan + connect via the C6 ---- */

/* Close the connect dialog. Safe from an event cb (async delete). */
static void conn_close(void)
{
    if (s_conn_kb)    { lv_obj_delete_async(s_conn_kb);    s_conn_kb = NULL; }
    if (s_conn_panel) { lv_obj_delete_async(s_conn_panel); s_conn_panel = NULL; }
    s_conn_pw = NULL;
}
static void conn_cancel_cb(lv_event_t *e) { (void)e; conn_close(); }

/* Applied on the LVGL thread after a connect attempt finishes. */
static void conn_result_apply(void *p)
{
    (void)p;
    if (s_wifi_status) lv_label_set_text(s_wifi_status, s_conn_result);
}

/* Off the LVGL thread: associate + wait for an IP. arg = heap copy of password. */
static void connect_task(void *arg)
{
    char *pw = (char *)arg;
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, s_sel_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pw ? pw : "", sizeof(cfg.sta.password));
    free(pw);

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    bool ok = false;
    esp_netif_ip_info_t ip = { 0 };
    if (esp_wifi_connect() == ESP_OK) {
        for (int i = 0; i < 40; i++) {           /* up to ~10s for assoc + DHCP */
            vTaskDelay(pdMS_TO_TICKS(250));
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) continue;
            esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) { ok = true; break; }
        }
    }
    if (ok) snprintf(s_conn_result, sizeof(s_conn_result), "connected: " IPSTR, IP2STR(&ip.ip));
    else    snprintf(s_conn_result, sizeof(s_conn_result), "connect failed: %s", s_sel_ssid);
    s_connecting = false;
    lv_async_call(conn_result_apply, NULL);
    vTaskDelete(NULL);
}

static void conn_do_cb(lv_event_t *e)
{
    (void)e;
    if (s_connecting) return;
    const char *pw = s_conn_pw ? lv_textarea_get_text(s_conn_pw) : "";
    char *pwcopy = strdup(pw ? pw : "");
    conn_close();                               /* tear down dialog (async) */
    if (s_wifi_status) lv_label_set_text(s_wifi_status, "connecting...");
    s_connecting = true;
    if (!pwcopy || xTaskCreate(connect_task, "wifi_conn", 4096, pwcopy, 4, NULL) != pdPASS) {
        free(pwcopy);
        s_connecting = false;
        if (s_wifi_status) lv_label_set_text(s_wifi_status, "connect: busy");
    }
}

/* Tap an AP row -> connect dialog (SSID + password + keyboard). */
static void ap_row_cb(lv_event_t *e)
{
    if (s_conn_panel) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_wifi_n) return;
    strlcpy(s_sel_ssid, (char *)s_wifi_recs[idx].ssid, sizeof(s_sel_ssid));

    s_conn_panel = lv_obj_create(s_overlay);
    lv_obj_set_size(s_conn_panel, 560, 230);
    lv_obj_align(s_conn_panel, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_color(s_conn_panel, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(s_conn_panel, lv_color_hex(HC_CYAN), 0);
    lv_obj_set_style_border_width(s_conn_panel, 1, 0);
    lv_obj_set_style_radius(s_conn_panel, 10, 0);
    lv_obj_remove_flag(s_conn_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(s_conn_panel);
    char hdr[64]; snprintf(hdr, sizeof(hdr), "connect to %s", s_sel_ssid);
    lv_label_set_text(t, hdr);
    lv_obj_set_style_text_color(t, lv_color_hex(HC_HONEY), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 6, 6);

    s_conn_pw = lv_textarea_create(s_conn_panel);
    lv_textarea_set_one_line(s_conn_pw, true);
    lv_textarea_set_password_mode(s_conn_pw, true);
    lv_textarea_set_placeholder_text(s_conn_pw, "password (blank = open)");
    lv_obj_set_width(s_conn_pw, LV_PCT(94));
    lv_obj_align(s_conn_pw, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *go = lv_button_create(s_conn_panel);
    lv_obj_set_size(go, 150, 52);
    lv_obj_align(go, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(go, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_border_color(go, lv_color_hex(HC_GREEN), 0);
    lv_obj_set_style_border_width(go, 1, 0);
    lv_obj_add_event_cb(go, conn_do_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(go); lv_label_set_text(gl, "connect");
    lv_obj_set_style_text_color(gl, lv_color_hex(HC_GREEN), 0); lv_obj_center(gl);

    lv_obj_t *cx = lv_button_create(s_conn_panel);
    lv_obj_set_size(cx, 150, 52);
    lv_obj_align(cx, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(cx, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_border_color(cx, lv_color_hex(HC_DIMTXT), 0);
    lv_obj_set_style_border_width(cx, 1, 0);
    lv_obj_add_event_cb(cx, conn_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cx); lv_label_set_text(cl, "cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(HC_DIMTXT), 0); lv_obj_center(cl);

    /* Keyboard parented on the overlay (bottom), bound to the password field. */
    s_conn_kb = lv_keyboard_create(s_overlay);
    lv_keyboard_set_textarea(s_conn_kb, s_conn_pw);
}

/* Runs on the LVGL thread (via lv_async). Fills the list from s_wifi_recs. */
static void wifi_populate(void *p)
{
    (void)p;
    if (!s_wifi_list || !s_wifi_status) return;    /* explorer closed meanwhile */
    lv_obj_clean(s_wifi_list);

    if (s_wifi_n == 0) {
        lv_obj_t *e = lv_label_create(s_wifi_list);
        lv_label_set_text(e, "no networks found");
        lv_obj_set_style_text_color(e, lv_color_hex(HC_DIMTXT), 0);
    }
    for (int i = 0; i < s_wifi_n; i++) {
        lv_obj_t *row = lv_obj_create(s_wifi_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(HC_PANEL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ap_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char line[80];
        snprintf(line, sizeof(line), "%4d dBm  ch%-2d  %s",
                 s_wifi_recs[i].rssi, s_wifi_recs[i].primary,
                 (char *)s_wifi_recs[i].ssid);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, line);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        /* stronger signal = warmer colour */
        uint32_t col = s_wifi_recs[i].rssi > -60 ? HC_HONEY :
                       s_wifi_recs[i].rssi > -75 ? HC_LHONEY : HC_DIMTXT;
        lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    }

    char st[32];
    snprintf(st, sizeof(st), "%u networks", s_wifi_n);
    lv_label_set_text(s_wifi_status, st);
}

/* Off the LVGL thread: block on the scan, then hand results to wifi_populate. */
static void wifi_scan_task(void *arg)
{
    (void)arg;
    uint16_t n = 0;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_num(&n) == ESP_OK) {
        uint16_t got = n < 30 ? n : 30;
        if (got && esp_wifi_scan_get_ap_records(&got, s_wifi_recs) == ESP_OK)
            s_wifi_n = got;
        else
            s_wifi_n = 0;
    } else {
        s_wifi_n = 0;
    }
    s_wifi_scanning = false;
    lv_async_call(wifi_populate, NULL);
    vTaskDelete(NULL);
}

static void wifi_start_scan(void)
{
    if (s_wifi_scanning) return;
    s_wifi_scanning = true;
    if (s_wifi_status) lv_label_set_text(s_wifi_status, "scanning...");
    if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 4, NULL) != pdPASS)
        s_wifi_scanning = false;
}

static void wifi_rescan_cb(lv_event_t *e) { (void)e; wifi_start_scan(); }

static void open_explorer(void)
{
    lv_obj_t *ov = tool_shell("explorer");

    s_wifi_status = lv_label_create(ov);
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(HC_CYAN), 0);
    lv_obj_align(s_wifi_status, LV_ALIGN_TOP_LEFT, 44, 100);
    lv_label_set_text(s_wifi_status, "scanning...");

    lv_obj_t *rescan = lv_button_create(ov);
    lv_obj_set_size(rescan, 120, 44);
    lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, -44, 92);
    lv_obj_set_style_bg_color(rescan, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(rescan, lv_color_hex(HC_CYAN), 0);
    lv_obj_set_style_border_width(rescan, 1, 0);
    lv_obj_add_event_cb(rescan, wifi_rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rescan);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH "  scan");
    lv_obj_set_style_text_color(rl, lv_color_hex(HC_CYAN), 0);
    lv_obj_center(rl);

    s_wifi_list = lv_obj_create(ov);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_size(s_wifi_list, LV_PCT(88), 540);
    lv_obj_align(s_wifi_list, LV_ALIGN_TOP_LEFT, 44, 140);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 8, 0);
    lv_obj_set_style_radius(s_wifi_list, 8, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);

    wifi_start_scan();
}

/* ---- ble: live BLE device scan via the C6 ---- */

/* Timer on the LVGL thread: refresh the device list from hc_ble_get(). */
static void ble_refresh(lv_timer_t *t)
{
    (void)t;
    if (!s_ble_list) return;

    static hc_ble_dev_t devs[48];
    int n = hc_ble_get(devs, 48);

    lv_obj_clean(s_ble_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(s_ble_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(HC_PANEL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_left(row, 8, 0);

        char line[80];
        const uint8_t *a = devs[i].addr;
        snprintf(line, sizeof(line), "%4d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %s",
                 devs[i].rssi, a[5], a[4], a[3], a[2], a[1], a[0], devs[i].name);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, line);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        uint32_t col = devs[i].rssi > -60 ? HC_HONEY :
                       devs[i].rssi > -80 ? HC_LHONEY : HC_DIMTXT;
        lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    }

    if (s_ble_status) {
        char st[32];
        snprintf(st, sizeof(st), "%s%d devices",
                 hc_ble_scanning() ? "scanning... " : "", n);
        lv_label_set_text(s_ble_status, st);
    }
}

static void ble_rescan_cb(lv_event_t *e) { (void)e; hc_ble_scan(10000); }

static void open_ble(void)
{
    lv_obj_t *ov = tool_shell("ble");

    s_ble_status = lv_label_create(ov);
    lv_obj_set_style_text_color(s_ble_status, lv_color_hex(HC_CYAN), 0);
    lv_obj_align(s_ble_status, LV_ALIGN_TOP_LEFT, 44, 100);
    lv_label_set_text(s_ble_status, hc_ble_ready() ? "scanning..." : "BLE not available");

    lv_obj_t *rescan = lv_button_create(ov);
    lv_obj_set_size(rescan, 120, 44);
    lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, -44, 92);
    lv_obj_set_style_bg_color(rescan, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_border_color(rescan, lv_color_hex(HC_CYAN), 0);
    lv_obj_set_style_border_width(rescan, 1, 0);
    lv_obj_add_event_cb(rescan, ble_rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rescan);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH "  scan");
    lv_obj_set_style_text_color(rl, lv_color_hex(HC_CYAN), 0);
    lv_obj_center(rl);

    s_ble_list = lv_obj_create(ov);
    lv_obj_remove_style_all(s_ble_list);
    lv_obj_set_size(s_ble_list, LV_PCT(88), 540);
    lv_obj_align(s_ble_list, LV_ALIGN_TOP_LEFT, 44, 140);
    lv_obj_set_style_bg_color(s_ble_list, lv_color_hex(HC_STRIPE), 0);
    lv_obj_set_style_bg_opa(s_ble_list, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(s_ble_list, 8, 0);
    lv_obj_set_style_radius(s_ble_list, 8, 0);
    lv_obj_set_flex_flow(s_ble_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ble_list, 6, 0);
    lv_obj_set_scroll_dir(s_ble_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ble_list, LV_SCROLLBAR_MODE_AUTO);

    if (hc_ble_ready()) hc_ble_scan(10000);         /* 10s observer scan */
    s_tool_timer = lv_timer_create(ble_refresh, 600, NULL);  /* live refresh */
}

static void async_open_tool(void *p)
{
    s_tool_pending = false;
    if (s_overlay) return;
    int id = (int)(intptr_t)p;
    switch (id) {
        case TOOL_CONFIG:   open_config(); break;
        case TOOL_BENCH:    open_bench(); break;
        case TOOL_FILES:    open_files(); break;
        case TOOL_EXPLORER: open_explorer(); break;
        case TOOL_BLE:      open_ble(); break;
        default:            open_msg("tool", "waiting on the C6 radio\nflash it via the M5 downloader", HC_DIMTXT); break;
    }
}

static void tile_click_cb(lv_event_t *e)
{
    if (s_overlay || s_tool_pending) return;
    s_tool_pending = true;
    lv_async_call(async_open_tool, lv_event_get_user_data(e));
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

    /* --- pannable comb (built as its own container so it can be freed while a
     * tool overlay is open, then rebuilt on close) --- */
    build_comb();

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

    /* Radio status dots — reflect the live state of each radio (green=on, red=off). */
    s_ble_dot = radio_dot(hdr, "BLE");
    lv_obj_align(lv_obj_get_parent(s_ble_dot), LV_ALIGN_RIGHT_MID, -100, 0);
    s_wifi_dot = radio_dot(hdr, "WIFI");
    lv_obj_align(lv_obj_get_parent(s_wifi_dot), LV_ALIGN_RIGHT_MID, -194, 0);

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

void hc_ui_set_radio(bool wifi_on, bool ble_on)
{
#if HAVE_LVGL
    if (!s_active) return;
    if (bsp_display_lock(50)) {
        if (s_wifi_dot)
            lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(wifi_on ? HC_GREEN : HC_RED), 0);
        if (s_ble_dot)
            lv_obj_set_style_bg_color(s_ble_dot, lv_color_hex(ble_on ? HC_GREEN : HC_RED), 0);
        bsp_display_unlock();
    }
#else
    (void)wifi_on; (void)ble_on;
#endif
}

/* Legacy mode setter: now just drives the Wi-Fi dot (ok -> green). */
void hc_ui_set_mode(hc_radio_mode_t mode, bool ok)
{
    (void)mode;
#if HAVE_LVGL
    if (!s_active) return;
    if (bsp_display_lock(50)) {
        if (s_wifi_dot)
            lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(ok ? HC_GREEN : HC_RED), 0);
        bsp_display_unlock();
    }
#else
    (void)ok;
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
