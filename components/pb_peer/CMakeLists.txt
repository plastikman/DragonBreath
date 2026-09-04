idf_component_register(
    SRCS "pb_peer.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_peer pb_policy pb_ntc esp_hw_support esp_app_format esp_netif
)
