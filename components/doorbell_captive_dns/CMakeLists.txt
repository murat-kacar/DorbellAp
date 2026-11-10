# Pure C, no ESP-IDF APIs: also built for the linux target by test/host_unit
idf_component_register(
    SRCS "src/doorbell_captive_dns.c"
    INCLUDE_DIRS "include"
)
