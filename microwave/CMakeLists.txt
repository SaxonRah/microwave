# RP2350 firmware build.
#
# Requires PICO_SDK_PATH. Configured by mw.bat:  .\mw.bat build pico
cmake_minimum_required(VERSION 3.16)

include(pico_sdk_import.cmake)

project(microwave C CXX ASM)
set(CMAKE_C_STANDARD 99)

pico_sdk_init()

add_executable(microwave
    main.c
    ../shared/src/snd.c
    ../shared/src/snd_synth.c
    ../shared/src/snd_seq.c
    ../shared/src/mw_music_demo.c)

target_include_directories(microwave PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../shared/src")

# snd_pack.c is deliberately absent: it needs stdio, and a bare-metal build
# that plays the synthesized demo has no file to read. Add it back, together
# with a filesystem, if you want packs on device.
target_link_libraries(microwave
    pico_stdlib
    hardware_dma
    hardware_pwm
    hardware_irq
    hardware_clocks)

pico_add_extra_outputs(microwave)
pico_enable_stdio_usb(microwave 1)
pico_enable_stdio_uart(microwave 0)
