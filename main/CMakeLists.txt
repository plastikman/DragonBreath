idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES pb_board pb_ntc pb_heater pb_fan pb_policy
)
