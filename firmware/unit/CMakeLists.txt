cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Shared firmware version of both roles (esp_app_desc and the protocol's fw_version_major/minor)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../version.txt" PROJECT_VER LIMIT_COUNT 1)

# Configuration shared by both roles. The resolved sdkconfig lives in the build directory, so a plain
# `idf.py build`, the flasher's Production build (build/) and its Burn build (build_burn/, + ../sdkconfig.burn) each
# have exactly one sdkconfig. The flasher and CI pass their own -D SDKCONFIG / SDKCONFIG_DEFAULTS layers.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults")
endif()
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doorbell_unit)
