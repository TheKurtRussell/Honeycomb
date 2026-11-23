idf_component_register(
    SRCS "hc_c6ota.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition esp_app_format esp_hosted
)
