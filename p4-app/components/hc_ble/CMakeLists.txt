idf_component_register(
    SRCS "hc_ble.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_hosted hc_scanlog
)
