idf_component_register(
    SRCS "magpie_main.c" "magpie_protocol.c" "magpie_ble.c" "magpie_dashboard.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_timer nvs_flash
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/magpie_cjk_14.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/magpie_cjk_22.c"
)
