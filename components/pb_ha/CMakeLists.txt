idf_component_register(
    SRCS "pb_ha.c"
    INCLUDE_DIRS "include"
    # dc_mqtt owns the shared ESP-MQTT lifecycle; this product component owns Home
    # Assistant discovery, command mapping, policy leases, and state telemetry.
    REQUIRES nvs_flash dc_mqtt esp_timer pb_policy
)
