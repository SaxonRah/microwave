cmake_minimum_required(VERSION 3.16)
project(microwave_shared C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MW_SND_U8 "Build the standalone mixer in legacy 8-bit sample mode" OFF)
option(MW_BUILD_SEQUENCER "Also build the optional pattern sequencer layer" OFF)
option(MW_BUILD_PACK "Also build the pack reader (needs stdio)" OFF)

# This is the actual mixer. It deliberately contains no clock, no device, no
# song, no stress test, no DOS and no Pico code -- the same division
# MicroRender draws around microrender_gfx.
add_library(microwave_snd STATIC
    src/snd.c
    src/snd_synth.c)

add_library(microwave::snd ALIAS microwave_snd)
target_include_directories(microwave_snd PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")

if(MW_SND_U8)
    target_compile_definitions(microwave_snd PUBLIC SND_SAMPLE_U8=1)
endif()

set(MW_WARNINGS_GNU
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef)

if(MSVC)
    target_compile_options(microwave_snd PRIVATE /W4)
else()
    target_compile_options(microwave_snd PRIVATE ${MW_WARNINGS_GNU})
endif()

# The sequencer is a separate target on purpose: an application can use the
# mixer without also accepting a tracker's idea of what a song is.
if(MW_BUILD_SEQUENCER)
    add_library(microwave_seq STATIC src/snd_seq.c)
    add_library(microwave::seq ALIAS microwave_seq)
    target_include_directories(microwave_seq PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_seq PUBLIC microwave_snd)
    if(MSVC)
        target_compile_options(microwave_seq PRIVATE /W4)
    else()
        target_compile_options(microwave_seq PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# The pack reader pulls in stdio, which a bare-metal Pico build may not want.
if(MW_BUILD_PACK)
    add_library(microwave_pack STATIC src/snd_pack.c)
    add_library(microwave::pack ALIAS microwave_pack)
    target_include_directories(microwave_pack PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_pack PUBLIC microwave_snd)
    if(MSVC)
        target_compile_options(microwave_pack PRIVATE /W4)
    else()
        target_compile_options(microwave_pack PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()
