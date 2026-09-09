cmake_minimum_required(VERSION 3.16)
project(microwave_tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# The validator writes a conventional PCM16 WAV. Keep the core build in its
# normal logical sample format even if another checkout has cached an alternate
# standalone mixer option elsewhere.
set(MW_SND_U8 OFF CACHE BOOL "" FORCE)
set(MW_BUILD_MUS ON CACHE BOOL "" FORCE)
set(MW_BUILD_MIDI_FM ON CACHE BOOL "" FORCE)
set(MW_BUILD_GENMIDI ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../shared"
                 "${CMAKE_BINARY_DIR}/microwave-shared")

add_executable(mw_genmidi_wav mw_genmidi_wav.c)
target_link_libraries(mw_genmidi_wav PRIVATE microwave::mus microwave::genmidi)

if(MSVC)
    # Keep the hosted tool portable C99. MSVC's C4996 warning is about its
    # preferred *_s API, not a problem with standard fopen().
    target_compile_definitions(mw_genmidi_wav PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mw_genmidi_wav PRIVATE /W4)
else()
    target_compile_options(mw_genmidi_wav PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef)
endif()
