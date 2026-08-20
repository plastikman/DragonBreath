idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES pb_board pb_ntc pb_heater pb_fan pb_policy pb_leds pb_buttons pb_hil pb_httpd db_portal
             dc_wifi dc_moonraker dc_source dc_bambu dc_prusa dc_ui pb_ha db_klipper_mqtt dc_evlog nvs_flash esp_wifi app_update
)
