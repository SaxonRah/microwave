cmake_minimum_required(VERSION 3.16)
project(microwave_raylib C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Raylib resolution, in the same order and for the same reasons as
# MicroRender's microrender_raylib/CMakeLists.txt:
#   1. an explicit source override
#   2. the pinned submodule, so a fresh clone builds a known-good version
#   3. an installed CONFIG package
#   4. a bare header/library pair anywhere the toolchain can see
# There is deliberately no RAYLIB_PATH environment variable. A build system
# that already knows how to find a dependency should not need a batch file to
# tell it.
set(MW_RAYLIB_PATH "" CACHE PATH "Optional override path to a raylib source checkout")
set(MW_RAYLIB_SUBMODULE_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/raylib")

set(MW_RAYLIB_RATE 22050 CACHE STRING "Mix rate in Hz")
set(MW_RAYLIB_BLOCK 1024 CACHE STRING "Frames per mix block")
set(MW_RAYLIB_CHANNELS 1 CACHE STRING "Output channels: 1 or 2")
option(MW_RAYLIB_HEADLESS "Build with the audio device compiled out" OFF)

if(NOT MW_RAYLIB_CHANNELS MATCHES "^(1|2)$")
    message(FATAL_ERROR "MW_RAYLIB_CHANNELS must be 1 or 2")
endif()

if(MW_RAYLIB_HEADLESS)
    message(STATUS "MicroWave Raylib: headless, no audio device required")
elseif(MW_RAYLIB_PATH)
    message(STATUS "MicroWave Raylib: using override source ${MW_RAYLIB_PATH}")
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${MW_RAYLIB_PATH}" "${CMAKE_BINARY_DIR}/raylib" EXCLUDE_FROM_ALL)
elseif(EXISTS "${MW_RAYLIB_SUBMODULE_DIR}/CMakeLists.txt")
    message(STATUS "MicroWave Raylib: using pinned submodule ${MW_RAYLIB_SUBMODULE_DIR}")
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${MW_RAYLIB_SUBMODULE_DIR}" "${CMAKE_BINARY_DIR}/raylib" EXCLUDE_FROM_ALL)
else()
    find_package(raylib CONFIG QUIET)
endif()

if(NOT MW_RAYLIB_HEADLESS AND NOT TARGET raylib)
    find_path(RAYLIB_INCLUDE_DIR raylib.h)
    find_library(RAYLIB_LIBRARY NAMES raylib)
    if(RAYLIB_INCLUDE_DIR AND RAYLIB_LIBRARY)
        add_library(raylib UNKNOWN IMPORTED)
        set_target_properties(raylib PROPERTIES
            IMPORTED_LOCATION "${RAYLIB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${RAYLIB_INCLUDE_DIR}")
    endif()
endif()

if(NOT MW_RAYLIB_HEADLESS AND NOT TARGET raylib)
    message(FATAL_ERROR
        "raylib was not found. Initialize the pinned submodule with: "
        "git submodule update --init --recursive. "
        "An installed package or -DMW_RAYLIB_PATH=C:/path/to/raylib can still "
        "be used as an override. To build without an audio device entirely, "
        "configure with -DMW_RAYLIB_HEADLESS=ON.")
endif()

set(MW_SHARED_SRC "${CMAKE_CURRENT_LIST_DIR}/../shared/src")

add_executable(microwave_raylib
    main.c
    "${MW_SHARED_SRC}/snd.c"
    "${MW_SHARED_SRC}/snd_synth.c"
    "${MW_SHARED_SRC}/snd_seq.c"
    "${MW_SHARED_SRC}/snd_pack.c"
    "${MW_SHARED_SRC}/mw_music_demo.c")

target_include_directories(microwave_raylib PRIVATE "${MW_SHARED_SRC}")

target_compile_definitions(microwave_raylib PRIVATE
    MW_RATE=${MW_RAYLIB_RATE}
    MW_BLOCK=${MW_RAYLIB_BLOCK}
    MW_CHANNELS=${MW_RAYLIB_CHANNELS})

if(MW_RAYLIB_HEADLESS)
    target_compile_definitions(microwave_raylib PRIVATE MW_HEADLESS=1)
else()
    target_link_libraries(microwave_raylib PRIVATE raylib)
endif()

if(MSVC)
    # fopen is the portable API the pack reader and the WAV writer use. C4996
    # is MSVC's vendor deprecation warning, so keep the portable call and
    # silence the warning for this frontend only.
    target_compile_definitions(microwave_raylib PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(microwave_raylib PRIVATE /O2 /W4)
else()
    target_compile_options(microwave_raylib PRIVATE -O2 -Wall -Wextra -Wpedantic)
endif()

message(STATUS "MicroWave Raylib: ${MW_RAYLIB_RATE} Hz, ${MW_RAYLIB_BLOCK}-frame blocks, "
               "${MW_RAYLIB_CHANNELS}ch, headless=${MW_RAYLIB_HEADLESS}")
