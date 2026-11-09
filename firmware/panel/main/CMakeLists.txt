idf_component_register(
    SRCS "main.c" "panel_keypad.c" "panel_portal.c" "panel_console.c"
    INCLUDE_DIRS "."
    REQUIRES doorbell_common doorbell_protocol doorbell_storage doorbell_led doorbell_net
             esp_http_server console esp_timer esp_hw_support esp_system esp_rom nvs_flash mbedtls
)

