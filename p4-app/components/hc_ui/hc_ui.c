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

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

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
#define COMB_COLS 8
#define COMB_ROWS 11

/* One shared outline shape; every cell reuses it (relative points). */
static const lv_point_precise_t HEXP[] = {
    {37, 0}, {113, 0}, {150, 65}, {113, 130}, {37, 130}, {0, 65}, {37, 0}
};

static lv_obj_t *s_mode_lbl;
static lv_obj_t *s_frames_lbl;
static uint32_t  s_frame_count;

/* --- free 2D drag-to-pan with momentum ---
 * LVGL's native scroll locks to one axis per gesture; we want map-style
 * diagonal panning, so we drive lv_obj_scroll_by() ourselves from the pointer
 * vector and let a decaying timer carry the fling after release. */
static lv_obj_t  *s_pan;
static lv_timer_t *s_fling;
static int s_vx, s_vy;

static void fling_cb(lv_timer_t *t)
{
    s_vx = (s_vx * 9) / 10;
    s_vy = (s_vy * 9) / 10;
    if (s_pan) lv_obj_scroll_by(s_pan, s_vx, s_vy, LV_ANIM_OFF);
    if (s_vx > -2 && s_vx < 2 && s_vy > -2 && s_vy < 2) { lv_timer_delete(t); s_fling = NULL; }
}

static void pan_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (s_fling) { lv_timer_delete(s_fling); s_fling = NULL; }
        s_vx = s_vy = 0;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        lv_obj_scroll_by(s_pan, v.x, v.y, LV_ANIM_OFF);
        s_vx = v.x; s_vy = v.y;
    } else if (code == LV_EVENT_RELEASED) {
        if ((s_vx > 3 || s_vx < -3 || s_vy > 3 || s_vy < -3) && !s_fling)
            s_fling = lv_timer_create(fling_cb, 16, NULL);
    }
}

typedef struct {
    int col, row;
    const char *icon;
    const char *name;
    uint32_t line_color;
    uint32_t text_color;
} hc_tile_t;

/* The six live tools; every other cell is an empty comb cell. */
static const hc_tile_t s_tiles[] = {
    {1, 1, LV_SYMBOL_DOWNLOAD, "bench",    HC_HONEY, HC_LHONEY},
    {2, 1, LV_SYMBOL_WIFI,     "explorer", HC_CYAN,  HC_LCYAN },
    {2, 2, LV_SYMBOL_SAVE,     "capture",  HC_HONEY, HC_LHONEY},
    {3, 1, LV_SYMBOL_LIST,     "decode",   HC_HONEY, HC_LHONEY},
    {3, 2, LV_SYMBOL_SHUFFLE,  "mesh",     HC_CYAN,  HC_LCYAN },
    {4, 1, LV_SYMBOL_SETTINGS, "config",   HC_HONEY, HC_LHONEY},
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
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);  /* custom free-pan, not axis-locked scroll */
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);      /* so it receives press/drag events */
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(cont, pan_cb, LV_EVENT_ALL, NULL);
    s_pan = cont;

    lv_obj_t *field = bare(cont);
    lv_obj_set_size(field, COMB_OX * 2 + COMB_COLS * HEX_AX + HEX_W / 4,
                           COMB_OY + COMB_ROWS * HEX_AY + HEX_H);

    for (int c = 0; c < COMB_COLS; c++) {
        for (int r = 0; r < COMB_ROWS; r++) {
            int x = COMB_OX + c * HEX_AX;
            int y = COMB_OY + r * HEX_AY + ((c & 1) ? HEX_H / 2 : 0);
            const hc_tile_t *t = find_tile(c, r);

            lv_obj_t *cell = bare(field);
            lv_obj_set_pos(cell, x, y);
            lv_obj_set_size(cell, HEX_W, HEX_H);

            lv_obj_t *hex = lv_line_create(cell);
            lv_line_set_points(hex, HEXP, 7);
            lv_obj_set_style_line_color(hex, lv_color_hex(t ? t->line_color : HC_DIMLINE), 0);
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

void hc_ui_set_flash_progress(const hc_flash_progress_t *p) { (void)p; }
void hc_ui_log(const char *line) { (void)line; }
