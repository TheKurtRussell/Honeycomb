/*
 * Honeycomb — P4 UI (LVGL) — Cyberpunk 2077 theme
 *
 * First-light build: a themed status screen that proves the display + theme are
 * alive, using only core LVGL objects (robust across LVGL 9.x). The full tabbed
 * Bench/Explorer interface layers on once we confirm pixels.
 *
 * Palette: near-black base, electric yellow primary, cyan + magenta accents,
 * angular (radius 0) neon-bordered panels.
 *
 * Threading: LVGL runs in the BSP's lvgl_port task, so every touch of an LVGL
 * object is wrapped in bsp_display_lock()/bsp_display_unlock().
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

/* ---- Cyberpunk palette ---- */
#define HC_BG      0x0A0A0F
#define HC_PANEL   0x14141C
#define HC_YELLOW  0xFCEE09
#define HC_CYAN    0x00F0FF
#define HC_MAGENTA 0xFF003C
#define HC_DIM     0x8A8AA0

static bool s_active;

#if HAVE_LVGL
static lv_obj_t *s_mode_lbl;
static lv_obj_t *s_frames_lbl;
static uint32_t  s_frame_count;

/* Make an angular neon-bordered panel. */
static lv_obj_t *panel(lv_obj_t *parent, uint32_t border)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, lv_color_hex(HC_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 0, 0);          /* angular */
    lv_obj_set_style_pad_all(p, 12, 0);
    return p;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}
#endif /* HAVE_LVGL */

esp_err_t hc_ui_init(void *lv_display)
{
    if (!lv_display) { ESP_LOGW(TAG, "headless UI"); return ESP_OK; }
#if HAVE_LVGL
    if (!bsp_display_lock(500)) return ESP_FAIL;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(HC_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Header */
    lv_obj_t *hdr = panel(scr, HC_YELLOW);
    lv_obj_set_width(hdr, lv_pct(94));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *title = label(hdr, "// HONEYCOMB", HC_YELLOW);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *sub = label(hdr, "M5STACK TAB5  \xC2\xB7  802.15.4 EXPLORER", HC_CYAN);
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    /* Status panel */
    lv_obj_t *st = panel(scr, HC_CYAN);
    lv_obj_set_width(st, lv_pct(94));
    lv_obj_set_height(st, LV_SIZE_CONTENT);
    lv_obj_align_to(st, hdr, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    label(st, "SYSTEM", HC_DIM);
    lv_obj_t *d = label(st, "DISPLAY .......... ONLINE", HC_YELLOW);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 0, 24);
    s_mode_lbl = label(st, "C6 RADIO ......... OFFLINE", HC_MAGENTA);
    lv_obj_align_to(s_mode_lbl, d, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    s_frames_lbl = label(st, "FRAMES ........... 0", HC_CYAN);
    lv_obj_align_to(s_frames_lbl, s_mode_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    /* Footer hint */
    lv_obj_t *foot = label(scr, "awaiting C6 \xC2\xB7 flash via M5 downloader", HC_DIM);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -16);

    bsp_display_unlock();
    s_active = true;
    ESP_LOGI(TAG, "cyberpunk UI up");
#endif
    return ESP_OK;
}

void hc_ui_add_frame(const hc_dissected_t *row)
{
#if HAVE_LVGL
    if (!s_active || !row) return;
    s_frame_count++;
    if (bsp_display_lock(50)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "FRAMES ........... %lu", (unsigned long)s_frame_count);
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
    const char *n = mode == HC_MODE_SNIFFER ? "SNIFFER" :
                    mode == HC_MODE_WIFI ? "WIFI" : "IDLE";
    if (bsp_display_lock(50)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "C6 RADIO ......... %s", ok ? n : "FAIL");
        lv_label_set_text(s_mode_lbl, buf);
        lv_obj_set_style_text_color(s_mode_lbl, lv_color_hex(ok ? HC_YELLOW : HC_MAGENTA), 0);
        bsp_display_unlock();
    }
#else
    (void)mode; (void)ok;
#endif
}

void hc_ui_set_flash_progress(const hc_flash_progress_t *p) { (void)p; }
void hc_ui_log(const char *line) { (void)line; }
