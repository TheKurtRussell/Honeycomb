idf_component_register(
    SRCS "hc_mesh.c"
    INCLUDE_DIRS "."
    REQUIRES hc_scanlog esp_timer esp_wifi esp_netif lwip
)
