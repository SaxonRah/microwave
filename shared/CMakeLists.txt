cmake_minimum_required(VERSION 3.16)
project(microwave_shared C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MW_SND_U8 "Build the standalone mixer in legacy 8-bit sample mode" OFF)
option(MW_BUILD_SEQUENCER "Also build the optional pattern sequencer layer" OFF)
option(MW_BUILD_MIDI "Also build the optional MIDI message/state layer" OFF)
option(MW_BUILD_MUS "Also build the optional MUS source adapter" OFF)
option(MW_BUILD_MIDI_FM "Also build the optional two-operator MIDI FM backend" OFF)
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

# MIDI is a protocol/state layer, not a song parser and not a synthesizer.
# It deliberately has no dependency on microwave_snd so the same decoder can
# feed a software synth, OPL backend, or hardware MIDI transport.
if(MW_BUILD_MIDI OR MW_BUILD_MUS OR MW_BUILD_MIDI_FM)
    add_library(microwave_midi STATIC src/snd_midi.c)
    add_library(microwave::midi ALIAS microwave_midi)
    target_include_directories(microwave_midi PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
    if(MSVC)
        target_compile_options(microwave_midi PRIVATE /W4)
    else()
        target_compile_options(microwave_midi PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# MUS is a zero-allocation source-format adapter over caller-owned bytes.
# It depends on the MIDI message/state boundary but not on the mixer.
if(MW_BUILD_MUS)
    add_library(microwave_mus STATIC src/snd_mus.c)
    add_library(microwave::mus ALIAS microwave_mus)
    target_include_directories(microwave_mus PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_mus PUBLIC microwave_midi)
    if(MSVC)
        target_compile_options(microwave_mus PRIVATE /W4)
    else()
        target_compile_options(microwave_mus PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# OPL-shaped two-operator software FM. This is a MIDI sink and sample generator,
# not a source-format parser and not a register-accurate YM3812 emulator.
if(MW_BUILD_MIDI_FM)
    add_library(microwave_midi_fm STATIC src/snd_midi_fm.c)
    add_library(microwave::midi_fm ALIAS microwave_midi_fm)
    target_include_directories(microwave_midi_fm PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_midi_fm PUBLIC microwave_snd microwave_midi)
    if(MSVC)
        target_compile_options(microwave_midi_fm PRIVATE /W4)
    else()
        target_compile_options(microwave_midi_fm PRIVATE ${MW_WARNINGS_GNU})
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
