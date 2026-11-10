idf_component_register(
    SRCS "src/doorbell_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES doorbell_protocol
    PRIV_REQUIRES nvs_flash esp_rom
)

