idf_component_register(
    SRCS "pb_bambu.c"
    INCLUDE_DIRS "include"
    # esp-mqtt over TLS (esp-tls/mbedTLS) to the printer's on-device broker.
    # Reports are scanned with strstr/strtof (no cJSON). Built into ESP-IDF.
    REQUIRES nvs_flash mqtt esp-tls
)
