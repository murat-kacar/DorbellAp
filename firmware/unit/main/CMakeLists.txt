idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES doorbell_common doorbell_protocol doorbell_storage doorbell_led doorbell_net
             esp_timer esp_hw_support esp_system nvs_flash
)

