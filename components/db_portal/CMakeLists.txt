idf_component_register(
    SRCS "db_portal.c" "db_portal_config.c" "db_portal_assets.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_portal pb_httpd pb_policy dc_source dc_moonraker dc_bambu dc_prusa pb_ha
             db_klipper_mqtt nvs_flash json app_update
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/www/favicon.png" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/www/diagnostics.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/www/consequential-toggle.js" TEXT)
target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/www/control-diagnostics.js" TEXT)
