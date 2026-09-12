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
option(MW_BUILD_GENMIDI "Also build the optional generic GENMIDI adapter" OFF)
option(MW_BUILD_GENMIDI_OPL
    "Build the register-native GENMIDI + Nuked OPL backend" OFF)
option(MW_BUILD_DMX_SFX "Also build the Doom/DMX type-3 SFX adapter" OFF)
option(MW_BUILD_DMX_MIX "Also build optional Doom/DMX gain/pan helpers" OFF)
option(MW_BUILD_PACK "Also build the pack reader (needs stdio)" OFF)

# This is the actual mixer. It deliberately contains no clock, no device, no
# song, no stress test, no DOS and no Pico code.
add_library(microwave_snd STATIC
    src/snd.c
    src/snd_synth.c)

add_library(microwave::snd ALIAS microwave_snd)
target_include_directories(microwave_snd PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src")

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

if(MW_BUILD_SEQUENCER)
    add_library(microwave_seq STATIC src/snd_seq.c)
    add_library(microwave::seq ALIAS microwave_seq)
    target_include_directories(microwave_seq PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_seq PUBLIC microwave_snd)
    if(MSVC)
        target_compile_options(microwave_seq PRIVATE /W4)
    else()
        target_compile_options(microwave_seq PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# MIDI is the common event/state boundary. Both the lightweight generic FM
# synth and the register-native OPL backend consume it.
if(MW_BUILD_MIDI OR MW_BUILD_MUS OR MW_BUILD_MIDI_FM OR MW_BUILD_GENMIDI
   OR MW_BUILD_GENMIDI_OPL)
    add_library(microwave_midi STATIC src/snd_midi.c)
    add_library(microwave::midi ALIAS microwave_midi)
    target_include_directories(microwave_midi PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    if(MSVC)
        target_compile_options(microwave_midi PRIVATE /W4)
    else()
        target_compile_options(microwave_midi PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

if(MW_BUILD_MUS)
    add_library(microwave_mus STATIC src/snd_mus.c)
    add_library(microwave::mus ALIAS microwave_mus)
    target_include_directories(microwave_mus PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_mus PUBLIC microwave_midi)
    if(MSVC)
        target_compile_options(microwave_mus PRIVATE /W4)
    else()
        target_compile_options(microwave_mus PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# Lightweight generic two-operator software FM. It remains useful for generic
# MIDI and zero-asset synthesis, but it is not an OPL register emulator.
if(MW_BUILD_MIDI_FM OR MW_BUILD_GENMIDI)
    add_library(microwave_midi_fm STATIC src/snd_midi_fm.c)
    add_library(microwave::midi_fm ALIAS microwave_midi_fm)
    target_include_directories(microwave_midi_fm PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_midi_fm PUBLIC
        microwave_snd microwave_midi)
    if(MSVC)
        target_compile_options(microwave_midi_fm PRIVATE /W4)
    else()
        target_compile_options(microwave_midi_fm PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# Legacy/generic GENMIDI adapter. This translates GENMIDI into generic FM
# patches and is retained for compatibility and A/B testing.
if(MW_BUILD_GENMIDI)
    add_library(microwave_genmidi STATIC src/snd_genmidi.c)
    add_library(microwave::genmidi ALIAS microwave_genmidi)
    target_include_directories(microwave_genmidi PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_genmidi PUBLIC microwave_midi_fm)
    if(MSVC)
        target_compile_options(microwave_genmidi PRIVATE /W4)
    else()
        target_compile_options(microwave_genmidi PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# Accurate register-native Doom GENMIDI backend.
#
# Nuked OPL3 is intentionally a separate target and retains its LGPL-2.1+
# license. MicroWave's driver is not compiled by default. The host tool build
# enables it after scripts/mw_vendor_nuked_opl3.ps1 has placed the pinned
# upstream files under third_party/nuked-opl3.
if(MW_BUILD_GENMIDI_OPL)
    set(MW_NUKED_OPL3_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../third_party/nuked-opl3")

    if(NOT EXISTS "${MW_NUKED_OPL3_DIR}/opl3.c"
       OR NOT EXISTS "${MW_NUKED_OPL3_DIR}/opl3.h")
        message(FATAL_ERROR
            "Nuked OPL3 is missing. Run "
            "powershell -ExecutionPolicy Bypass -File "
            "scripts/mw_vendor_nuked_opl3.ps1")
    endif()

    add_library(microwave_nuked_opl3 STATIC
        "${MW_NUKED_OPL3_DIR}/opl3.c")
    add_library(microwave::nuked_opl3 ALIAS microwave_nuked_opl3)
    target_include_directories(microwave_nuked_opl3 PUBLIC
        "${MW_NUKED_OPL3_DIR}")
    target_compile_definitions(microwave_nuked_opl3 PRIVATE
        OPL_WF_TABLE_RUNTIME=1
        OPL_ENABLE_STEREOEXT=0)

    # Third-party source retains its own warning policy; do not make a new
    # compiler version turn upstream warnings into MicroWave build failures.
    if(MSVC)
        target_compile_options(microwave_nuked_opl3 PRIVATE /W3)
    else()
        target_compile_options(microwave_nuked_opl3 PRIVATE
            -Wall -Wextra)
    endif()

    add_library(microwave_genmidi_opl STATIC
        src/snd_genmidi_opl.c)
    add_library(microwave::genmidi_opl ALIAS microwave_genmidi_opl)
    target_include_directories(microwave_genmidi_opl PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src"
        "${MW_NUKED_OPL3_DIR}")
    target_link_libraries(microwave_genmidi_opl PUBLIC
        microwave_snd microwave_midi microwave_nuked_opl3)

    if(MSVC)
        target_compile_options(microwave_genmidi_opl PRIVATE /W4)
    else()
        target_compile_options(microwave_genmidi_opl PRIVATE
            ${MW_WARNINGS_GNU})
    endif()
endif()

# Generic Doom/DMX type-3 PCM adapter. The caller supplies the lump bytes and
# owns their lifetime; this layer deliberately contains no WAD API or I/O.
if(MW_BUILD_DMX_SFX)
    add_library(microwave_dmx_sfx STATIC src/snd_dmx_sfx.c)
    add_library(microwave::dmx_sfx ALIAS microwave_dmx_sfx)
    target_include_directories(microwave_dmx_sfx PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_dmx_sfx PUBLIC microwave_snd)
    if(MSVC)
        target_compile_options(microwave_dmx_sfx PRIVATE /W4)
    else()
        target_compile_options(microwave_dmx_sfx PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

# Historical DMX/Multivoc quantized gain/pan helpers are optional and separate
# from the byte decoder so applications can use either boundary independently.
if(MW_BUILD_DMX_MIX)
    add_library(microwave_dmx_mix STATIC src/snd_dmx_mix.c)
    add_library(microwave::dmx_mix ALIAS microwave_dmx_mix)
    target_include_directories(microwave_dmx_mix PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    if(MSVC)
        target_compile_options(microwave_dmx_mix PRIVATE /W4)
    else()
        target_compile_options(microwave_dmx_mix PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()

if(MW_BUILD_PACK)
    add_library(microwave_pack STATIC src/snd_pack.c)
    add_library(microwave::pack ALIAS microwave_pack)
    target_include_directories(microwave_pack PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(microwave_pack PUBLIC microwave_snd)
    if(MSVC)
        target_compile_options(microwave_pack PRIVATE /W4)
    else()
        target_compile_options(microwave_pack PRIVATE ${MW_WARNINGS_GNU})
    endif()
endif()
