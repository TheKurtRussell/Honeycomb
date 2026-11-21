idf_component_register(
    SRCS "hc_board.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_gpio
    PRIV_REQUIRES freertos
)
# Phase: add the Tab5 display BSP here once chosen, e.g.
#   REQUIRES ... esp_lcd esp_lcd_touch_gt911   (hand-rolled)
# or pull the m5stack_tab5 BSP via main/idf_component.yml and REQUIRES it.
