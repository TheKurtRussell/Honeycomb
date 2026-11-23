idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES hc_board hc_storage hc_explorer hc_mode_manager hc_flasher hc_ui
             hc_c6updater nvs_flash
             esp_wifi esp_netif esp_event esp_wifi_remote esp_hosted esp_partition bt
)
