# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if(EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pimoroni_pico_plus2_rp2350 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.16)
include(pico_sdk_import.cmake)
project(microwave C CXX ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Pico audio configuration
# ---------------------------------------------------------------------------

set(MW_PICO_DEVICE "MAX98357A" CACHE STRING
    "I2S sink: MAX98357A, PCM5102A, or NS4168")
set_property(CACHE MW_PICO_DEVICE PROPERTY STRINGS MAX98357A PCM5102A NS4168)
string(TOUPPER "${MW_PICO_DEVICE}" MW_PICO_DEVICE_UPPER)

if(MW_PICO_DEVICE_UPPER STREQUAL "MAX98357A")
    set(MW_PICO_DEVICE_ID 1)
elseif(MW_PICO_DEVICE_UPPER STREQUAL "PCM5102A")
    set(MW_PICO_DEVICE_ID 2)
elseif(MW_PICO_DEVICE_UPPER STREQUAL "NS4168")
    set(MW_PICO_DEVICE_ID 3)
else()
    message(FATAL_ERROR
        "MW_PICO_DEVICE must be MAX98357A, PCM5102A, or NS4168; got '${MW_PICO_DEVICE}'")
endif()

# 32 kHz is the common default for all three devices. In particular it is a
# documented 3-wire/BCK-PLL operating point for PCM5102A with 16-bit stereo
# slots (32 BCLKs per LRCLK), unlike MicroWave's historical 22050 Hz PWM rate.
set(MW_PICO_RATE 32000 CACHE STRING "I2S sample rate in Hz")
set(MW_PICO_BLOCK 256 CACHE STRING "Mixer frames per DMA block")
set(MW_PICO_SYS_KHZ 0 CACHE STRING
    "Optional RP2350 system clock in kHz; 0 keeps the SDK/board default")
option(MW_PICO_SERIAL "Enable USB serial status/PING service" ON)

set(MW_I2S_PIN_BCLK 10 CACHE STRING "I2S BCLK GPIO")
set(MW_I2S_PIN_LRCLK 11 CACHE STRING "I2S LRCLK GPIO; must equal BCLK + 1")
set(MW_I2S_PIN_DATA 12 CACHE STRING "I2S DATA/DIN/SDATA GPIO")
set(MW_I2S_PIO 0 CACHE STRING "PIO block: 0 or 1")
set(MW_I2S_SM 0 CACHE STRING "PIO state machine: 0..3")
set(MW_I2S_DMA 0 CACHE STRING "DMA channel")

foreach(v MW_PICO_RATE MW_PICO_BLOCK MW_PICO_SYS_KHZ
          MW_I2S_PIN_BCLK MW_I2S_PIN_LRCLK MW_I2S_PIN_DATA
          MW_I2S_PIO MW_I2S_SM MW_I2S_DMA)
    if(NOT "${${v}}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${v} must be a non-negative integer; got '${${v}}'")
    endif()
endforeach()

if(MW_PICO_BLOCK LESS 1)
    message(FATAL_ERROR "MW_PICO_BLOCK must be positive")
endif()
math(EXPR MW_I2S_EXPECTED_LRCLK "${MW_I2S_PIN_BCLK} + 1")
if(NOT MW_I2S_PIN_LRCLK EQUAL MW_I2S_EXPECTED_LRCLK)
    message(FATAL_ERROR
        "The PIO I2S program uses two adjacent side-set pins: "
        "MW_I2S_PIN_LRCLK must equal MW_I2S_PIN_BCLK + 1")
endif()
if(MW_I2S_PIO GREATER 1)
    message(FATAL_ERROR "MW_I2S_PIO must be 0 or 1")
endif()
if(MW_I2S_SM GREATER 3)
    message(FATAL_ERROR "MW_I2S_SM must be in 0..3")
endif()

if(MW_PICO_DEVICE_ID EQUAL 2)
    # PCM5102A's 3-wire mode synthesizes SCK from BCK. With the 16-bit stereo
    # transport below (32 BCLKs/LRCLK), keep overrides to documented BCK-PLL
    # sample-rate points rather than accepting a value that may never lock.
    if(NOT MW_PICO_RATE EQUAL 32000 AND
       NOT MW_PICO_RATE EQUAL 44100 AND
       NOT MW_PICO_RATE EQUAL 48000 AND
       NOT MW_PICO_RATE EQUAL 96000 AND
       NOT MW_PICO_RATE EQUAL 192000 AND
       NOT MW_PICO_RATE EQUAL 384000)
        message(FATAL_ERROR
            "PCM5102A three-wire mode with 32 BCLK/LRCLK supports this build at "
            "32000, 44100, 48000, 96000, 192000, or 384000 Hz; got ${MW_PICO_RATE}")
    endif()
else()
    if(MW_PICO_RATE LESS 8000 OR MW_PICO_RATE GREATER 96000)
        message(FATAL_ERROR
            "${MW_PICO_DEVICE_UPPER} sample rate must be in 8000..96000 Hz")
    endif()
endif()

add_executable(microwave
    main.c
    ../shared/src/snd.c
    ../shared/src/snd_synth.c
    ../shared/src/snd_seq.c
    ../shared/src/mw_music_demo.c)

pico_generate_pio_header(microwave
    ${CMAKE_CURRENT_LIST_DIR}/mw_i2s.pio)

target_include_directories(microwave PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../shared/src")

# snd_pack.c is deliberately absent: it needs stdio/filesystem services, while
# this firmware exercises the same synthesized demo as DOS and desktop.
target_link_libraries(microwave
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_pio)

target_compile_definitions(microwave PRIVATE
    MW_PICO_DEVICE=${MW_PICO_DEVICE_ID}
    MW_PICO_RATE=${MW_PICO_RATE}
    MW_PICO_BLOCK=${MW_PICO_BLOCK}
    MW_PICO_SYS_KHZ=${MW_PICO_SYS_KHZ}
    MW_I2S_PIN_BCLK=${MW_I2S_PIN_BCLK}
    MW_I2S_PIN_LRCLK=${MW_I2S_PIN_LRCLK}
    MW_I2S_PIN_DATA=${MW_I2S_PIN_DATA}
    MW_I2S_PIO=${MW_I2S_PIO}
    MW_I2S_SM=${MW_I2S_SM}
    MW_I2S_DMA=${MW_I2S_DMA}
    MW_PICO_SERIAL=$<BOOL:${MW_PICO_SERIAL}>)

pico_add_extra_outputs(microwave)
if(MW_PICO_SERIAL)
    pico_enable_stdio_usb(microwave 1)
else()
    pico_enable_stdio_usb(microwave 0)
endif()
pico_enable_stdio_uart(microwave 0)

message(STATUS
    "MicroWave Pico: device=${MW_PICO_DEVICE_UPPER}, rate=${MW_PICO_RATE} Hz, "
    "block=${MW_PICO_BLOCK}, BCLK=${MW_I2S_PIN_BCLK}, LRCLK=${MW_I2S_PIN_LRCLK}, "
    "DATA=${MW_I2S_PIN_DATA}, PIO=${MW_I2S_PIO}/${MW_I2S_SM}, DMA=${MW_I2S_DMA}, "
    "serial=${MW_PICO_SERIAL}")
