idf_component_register(
    SRCS "src/doorbell_common.c" "src/doorbell_twdt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer
    PRIV_REQUIRES esp_system
)
