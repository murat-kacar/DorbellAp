idf_component_register(
    SRCS "main.c" "panel_pairing.c" "panel_keypad.c" "panel_portal.c" "panel_console.c"
    INCLUDE_DIRS "."
    REQUIRES doorbell_common doorbell_protocol doorbell_storage doorbell_led doorbell_net doorbell_transport
             doorbell_keypad doorbell_captive_dns esp_http_server console esp_timer lwip nvs_flash mbedtls
)
