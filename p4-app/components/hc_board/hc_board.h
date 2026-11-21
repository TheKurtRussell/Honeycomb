/*
 * Honeycomb — M5Stack Tab5 board support (P4)
 *
 * Centralizes every Tab5 pin/peripheral the app touches, so hardware quirks
 * live in one file. Pin numbers come from the M5Stack Tab5 docs + NuttX port;
 * anything marked VERIFY should be checked against your board's schematic before
 * trusting it (M5 occasionally revises pinmaps between batches).
 */
#ifndef HC_BOARD_H
#define HC_BOARD_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- P4 <-> C6 SDIO2 bus (per M5 Tab5 pin map) --------------------------- */
/* D0..D3 / CMD / CK on G8..G13; RESET + IO2 on G14/G15. */
#define HC_PIN_C6_SDIO_D0    8
#define HC_PIN_C6_SDIO_D1    9
#define HC_PIN_C6_SDIO_D2    10
#define HC_PIN_C6_SDIO_D3    11
#define HC_PIN_C6_SDIO_CMD   12
#define HC_PIN_C6_SDIO_CLK   13
#define HC_PIN_C6_RESET      15   /* VERIFY: C6 EN/RESET line */
#define HC_PIN_C6_IO2        14

/* ---- microSD (SDMMC) — see hc_storage.c; pins reported as G39..G44 -------- */

/* Bring up display + touch + backlight (esp_lcd MIPI-DSI + GT911). Returns the
 * LVGL display handle via lv_disp internally; see hc_ui. */
esp_err_t hc_board_init(void);

/* Pulse the C6 reset line (used by the mode manager). */
void hc_board_c6_reset(void);

/* INA226 battery/bus gauge (Tab5 has one at 0x41). Returns millivolts/milliamps.
 * Handy to show runtime on the status bar. */
esp_err_t hc_board_power_read(int *mv, int *ma);

#ifdef __cplusplus
}
#endif
#endif /* HC_BOARD_H */
