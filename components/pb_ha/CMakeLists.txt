idf_component_register(
    SRCS "pb_ha.c"
    INCLUDE_DIRS "include"
    # esp-mqtt to the user's broker; esp_timer for pacing; pb_policy for the
    # command/snapshot API. All built into ESP-IDF / first-party — no third-party
    # dependency. State/discovery JSON is built with snprintf (no cJSON needed).
    REQUIRES nvs_flash mqtt esp_timer pb_policy
)
