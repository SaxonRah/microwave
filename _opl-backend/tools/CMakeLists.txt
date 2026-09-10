cmake_minimum_required(VERSION 3.16)
project(microwave_tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Both validators write conventional PCM16 WAV files.
set(MW_SND_U8 OFF CACHE BOOL "" FORCE)
set(MW_BUILD_MUS ON CACHE BOOL "" FORCE)

# Existing lightweight/generic path.
set(MW_BUILD_MIDI_FM ON CACHE BOOL "" FORCE)
set(MW_BUILD_GENMIDI ON CACHE BOOL "" FORCE)

# Register-native Doom OPL reference/runtime path.
set(MW_BUILD_GENMIDI_OPL ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../shared"
                 "${CMAKE_BINARY_DIR}/microwave-shared")

add_executable(mw_genmidi_wav mw_genmidi_wav.c)
target_link_libraries(mw_genmidi_wav PRIVATE
    microwave::mus microwave::genmidi)

add_executable(mw_opl_wav mw_opl_wav.c)
target_link_libraries(mw_opl_wav PRIVATE
    microwave::mus microwave::genmidi_opl)

foreach(target mw_genmidi_wav mw_opl_wav)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion
            -Wstrict-prototypes -Wmissing-prototypes
            -Wcast-qual -Wundef)
    endif()
endforeach()
