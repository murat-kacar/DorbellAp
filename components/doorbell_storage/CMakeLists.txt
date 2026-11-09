idf_component_register(
    SRCS "src/doorbell_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_rom doorbell_common doorbell_protocol
)

