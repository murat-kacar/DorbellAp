idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES doorbell_common doorbell_protocol doorbell_storage doorbell_led doorbell_net
             esp_timer lwip nvs_flash
)

