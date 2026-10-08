idf_component_register(
    SRCS "pb_httpd.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server pb_heater pb_ntc pb_leds pb_policy dc_source dc_bambu dc_moonraker dc_prusa dc_evlog json nvs_flash app_update
             esp_wifi esp_timer esp_system
)
