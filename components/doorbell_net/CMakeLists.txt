idf_component_register(
    SRCS "src/doorbell_net.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_netif esp_wifi esp_event esp_timer lwip doorbell_common doorbell_protocol
)
