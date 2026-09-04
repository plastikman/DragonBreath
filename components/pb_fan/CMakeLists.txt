idf_component_register(
    SRCS "pb_fan.c" "pb_fan_zc_filter.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_board driver esp_timer
)
