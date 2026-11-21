/*
 * Honeycomb — Tab5 board support implementation
 *
 * The C6 reset control is real (plain GPIO). Display/touch bring-up is the one
 * heavy board piece: the Tab5 panel is a 1280x720 MIPI-DSI panel with a GT911
 * touch controller. Two good ways to bring it up:
 *   (A) Use M5Stack's / the community esp-bsp "m5stack_tab5" BSP component,
 *       which gives you bsp_display_start() returning an lv_display_t*.
 *   (B) Hand-roll esp_lcd_mipi_dsi + esp_lcd_touch_gt911.
 * We default to (A) — add the BSP to main/idf_component.yml and call it here.
 */
#include "hc_board.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "hc_board"

void hc_board_c6_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << HC_PIN_C6_RESET,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(HC_PIN_C6_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(HC_PIN_C6_RESET, 1);
    ESP_LOGI(TAG, "C6 reset pulsed");
}

esp_err_t hc_board_init(void)
{
    /* TODO(display): call the Tab5 BSP here, e.g.
     *   bsp_i2c_init();
     *   lv_display_t *disp = bsp_display_start();
     *   bsp_display_backlight_on();
     * and hand `disp` to hc_ui_init(). Until wired, the app still runs headless
     * and logs to the console, so the link/sniffer/flasher are all testable. */
    ESP_LOGW(TAG, "display BSP not yet bound — running headless (see hc_board.c)");
    return ESP_OK;
}

esp_err_t hc_board_power_read(int *mv, int *ma)
{
    /* TODO: read INA226 @ 0x41 over the Tab5 I2C bus. */
    if (mv) *mv = 0; if (ma) *ma = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
