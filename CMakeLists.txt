cmake_minimum_required(VERSION 3.16)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(
    solar_os_picotts_source
    GIT_REPOSITORY https://github.com/DiUS/esp-picotts.git
    GIT_TAG bf1a8df9d2be1e088a03a775d439ac66e18438dc
    GIT_SHALLOW FALSE
    GIT_SUBMODULES ""
    SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/picotts"
)
FetchContent_GetProperties(solar_os_picotts_source)
if(NOT solar_os_picotts_source_POPULATED)
    FetchContent_Populate(solar_os_picotts_source)
endif()
set(SOLAR_OS_PICOTTS_SOURCE_DIR
    "${solar_os_picotts_source_SOURCE_DIR}"
    CACHE INTERNAL "Pinned PicoTTS source used by the SolarOS component"
)
set(ENV{SOLAR_OS_PICOTTS_SOURCE_DIR} "${solar_os_picotts_source_SOURCE_DIR}")
set(SOLAR_OS_PICOTTS_VOICE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/picotts_voices"
    CACHE INTERNAL "Versioned PicoTTS runtime voice directory"
)
set(ENV{SOLAR_OS_PICOTTS_VOICE_DIR} "${SOLAR_OS_PICOTTS_VOICE_DIR}")

# Heapdesk side project: opt-in only, stock builds are unaffected.
if("$ENV{SOLAR_OS_ENABLE_HEAPDESK}" MATCHES "^(1|[Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]|[Oo][Nn])$")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/heapdesk_components/heapdesk_connector")
    if(NOT DEFINED SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.heapdesk")
    if("$ENV{SOLAR_OS_HEAPDESK_COREDUMP}" MATCHES "^(1|[Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]|[Oo][Nn])$")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.heapdesk_coredump")
    endif()
    # Optional build-time credentials (NVS values set via the shell win).
    set(_hd_env "")
    foreach(_hd MQTT_URI API_URL DEVICE_ID DEVICE_SECRET)
        if(NOT "$ENV{HEAPDESK_${_hd}}" STREQUAL "")
            string(APPEND _hd_env "CONFIG_HEAPDESK_${_hd}=\"$ENV{HEAPDESK_${_hd}}\"\n")
        endif()
    endforeach()
    if(NOT _hd_env STREQUAL "")
        file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.defaults.heapdesk_env" "${_hd_env}")
        list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_BINARY_DIR}/sdkconfig.defaults.heapdesk_env")
    endif()
    message(STATUS "Heapdesk enabled; SDKCONFIG_DEFAULTS=${SDKCONFIG_DEFAULTS}")
endif()

include(patches/nimble/config_guard.cmake)
solar_os_regenerate_stale_sdkconfig()
include(scripts/cxx_sdkconfig.cmake)
solar_os_enable_required_cxx_exceptions()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "ESP_IDF_LWIP_HOOK_FILENAME=\"${CMAKE_CURRENT_LIST_DIR}/src/services/solar_os_lwip_hooks.h\""
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS "MEMP_NUM_PPP_PCB=2" APPEND)
project(solar_os)
include(patches/nimble/overlay.cmake)
