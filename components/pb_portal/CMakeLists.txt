idf_component_register(
    SRCS "pb_portal.c" "pb_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server dc_wifi dc_ui pb_httpd dc_source dc_moonraker dc_bambu pb_ha db_klipper_mqtt nvs_flash esp_wifi lwip json
)

# Favicon: a real 32x32 PNG of the DragonBreath dragon mark (pre-rendered, checked
# into www/ so the build needs no headless browser). Embedded into flash rodata and
# served verbatim as image/png at /favicon.ico, so the browser's automatic
# /favicon.ico fetch gets a proper icon instead of falling through to the SPA HTML.
target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/www/favicon.png" BINARY)
