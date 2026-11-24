idf_component_register(
    SRCS "magpie_main.c" "magpie_protocol.c" "magpie_ble.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_timer nvs_flash
)
