idf_component_register(
    SRCS "db_klipper_mqtt.c"
    INCLUDE_DIRS "include"
    # dc_mqtt owns the shared ESP-MQTT lifecycle; this product component owns the
    # Moonraker topic contract, retained-aware arming, and policy mapping.
    REQUIRES nvs_flash dc_mqtt esp_timer pb_policy
)
