idf_component_register(
    SRCS "db_portal.c" "db_portal_config.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_portal pb_httpd pb_policy dc_source dc_moonraker dc_bambu dc_prusa pb_ha
             db_klipper_mqtt nvs_flash json app_update
)

target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/www/favicon.png" BINARY)
