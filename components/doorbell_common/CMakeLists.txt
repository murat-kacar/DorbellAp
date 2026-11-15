idf_component_register(
    SRCS "src/doorbell_relay.c" "src/doorbell_reset_reason.c" "src/doorbell_twdt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer
    PRIV_REQUIRES esp_system
)

# Firmware version (DOORBELL_FW_VERSION_MAJOR / _MINOR, carried in PAIR_HELLO, PAIR_COMPLETE and HEARTBEAT_REQ) taken
# from the project's PROJECT_VER (firmware/version.txt), the value esp_app_desc reports too: one source of truth.
idf_build_get_property(doorbell_project_ver PROJECT_VER)
if(NOT doorbell_project_ver MATCHES "^([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "PROJECT_VER '${doorbell_project_ver}' must start with <major>.<minor> (firmware/version.txt)")
endif()
set(doorbell_fw_major ${CMAKE_MATCH_1})
set(doorbell_fw_minor ${CMAKE_MATCH_2})
if(doorbell_fw_major GREATER 255 OR doorbell_fw_minor GREATER 255)
    message(FATAL_ERROR "PROJECT_VER '${doorbell_project_ver}': major and minor must each fit in a uint8_t")
endif()
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    DOORBELL_FW_VERSION_MAJOR=${doorbell_fw_major}
    DOORBELL_FW_VERSION_MINOR=${doorbell_fw_minor})
