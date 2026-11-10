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
