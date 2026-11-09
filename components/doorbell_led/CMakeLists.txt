idf_component_register(
    SRCS "src/doorbell_led.c"
    INCLUDE_DIRS "include"
    REQUIRES doorbell_common esp_driver_gpio esp_driver_ledc esp_timer
)
