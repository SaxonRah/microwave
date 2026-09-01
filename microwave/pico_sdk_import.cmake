# Minimal external-project Pico SDK import used by MicroWave.
# Compatible with the PICO_SDK_PATH environment used by the Pico VS Code
# extension and by microwave/pico_env_auto.bat.

if(DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    message("Using PICO_SDK_PATH from environment ('${PICO_SDK_PATH}')")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to the Raspberry Pi Pico SDK")

if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR
        "PICO_SDK_PATH is not set. Run through mw.bat or set it explicitly.")
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH
                       BASE_DIR "${CMAKE_BINARY_DIR}")
set(PICO_SDK_INIT_CMAKE_FILE "${PICO_SDK_PATH}/pico_sdk_init.cmake")
if(NOT EXISTS "${PICO_SDK_INIT_CMAKE_FILE}")
    message(FATAL_ERROR
        "${PICO_SDK_PATH} does not contain pico_sdk_init.cmake")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to the Raspberry Pi Pico SDK" FORCE)
include("${PICO_SDK_INIT_CMAKE_FILE}")
