idf_component_register(
    SRCS "pb_fan.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_board driver esp_timer
)
