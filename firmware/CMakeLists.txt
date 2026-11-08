cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
option(METER_INCLUDE_LOCAL_CONFIG "Load ignored sdkconfig.local development defaults" ON)
if(METER_INCLUDE_LOCAL_CONFIG AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
endif()
set(METER_SDKCONFIG_PROFILE "" CACHE STRING
    "Optional sdkconfig defaults file appended after the normal defaults")
if(METER_SDKCONFIG_PROFILE)
    list(APPEND SDKCONFIG_DEFAULTS "${METER_SDKCONFIG_PROFILE}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(codex_usage_meter)
