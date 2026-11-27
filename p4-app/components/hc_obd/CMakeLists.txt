idf_component_register(
    SRCS "hc_obd.c"
    INCLUDE_DIRS "."
    REQUIRES hc_ble bt esp_timer
)
