idf_component_register(
    SRCS "src/doorbell_common.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer esp_hw_support esp_system
)
