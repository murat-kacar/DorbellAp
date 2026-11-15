idf_component_register(
    SRCS "main.c" "unit_buttons.c" "unit_pairing.c"
    INCLUDE_DIRS "."
    REQUIRES doorbell_common doorbell_protocol doorbell_storage doorbell_led doorbell_net doorbell_transport
             esp_timer lwip nvs_flash
)

