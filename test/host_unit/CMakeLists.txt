# Host unit tests of the pure components, built for ESP-IDF's linux target and run as a normal process:
#   idf.py --preview set-target linux && idf.py build && ./build/doorbell_host_unit.elf
cmake_minimum_required(VERSION 3.22)

# Only the components under test (doorbell_common & co. need ESP32 hardware)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/doorbell_protocol"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/doorbell_captive_dns")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(doorbell_host_unit)

# ESP-IDF v6.1 mbedtls on the linux target: libtfpsacrypto.a calls esp_mbedtls_mem_calloc/free (port/esp_mem.c in the
# component library) and mbedtls_ms_time (port/esp_platform_time.c in the mbedtls library), which GNU ld has already
# passed by then. Declaring the dependency makes CMake place both libraries after tfpsacrypto on the link line.
idf_component_get_property(mbedtls_component_lib mbedtls COMPONENT_LIB)
target_link_libraries(tfpsacrypto PRIVATE ${mbedtls_component_lib} mbedtls)
