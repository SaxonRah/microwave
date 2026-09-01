# MicroWave host test target.
#
# Standalone: this does not require the Pico SDK, Open Watcom, DOSBox or an
# audio device. It compiles the shared mixer for the host so the same code that
# ships to DOS and RP2350 can be unit tested, fuzzed and benchmarked in CI.
#
#   cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
#
# Sanitizers are on by default in Debug and off in Release, since the benchmark
# is meaningless under ASan. Override with -DMW_TESTS_SANITIZE=ON/OFF.

cmake_minimum_required(VERSION 3.16)
project(microwave_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(MW_SHARED_SRC "${CMAKE_CURRENT_LIST_DIR}/../shared/src")

if(NOT EXISTS "${MW_SHARED_SRC}/snd.c")
    message(FATAL_ERROR
        "Cannot find the shared mixer at ${MW_SHARED_SRC}. "
        "Configure this directory from the repository root, e.g. "
        "cmake -S tests -B build-tests")
endif()

# GCC and Clang sanitizer builds are the default in Debug. MSVC ASan is
# opt-in, matching MicroRender's reasoning about hosted Windows runners.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    set(MW_TESTS_SANITIZE_DEFAULT ON)
else()
    set(MW_TESTS_SANITIZE_DEFAULT OFF)
endif()

option(MW_TESTS_SANITIZE
    "Build the host tests with AddressSanitizer and UndefinedBehaviorSanitizer"
    ${MW_TESTS_SANITIZE_DEFAULT})
option(MW_TESTS_WERROR "Treat warnings in the shared mixer as errors" ON)
option(MW_TESTS_U8
    "Optional legacy 8-bit sample regression build; shipping frontends use S16"
    OFF)
option(MW_TESTS_NARROW
    "Force the DOS-style saturating mix path on a 32-bit host" OFF)

set(MW_FUZZ_ITERATIONS 200 CACHE STRING
    "Iterations for the fuzz harness when run through ctest")
set(MW_BENCH_SECONDS 5 CACHE STRING
    "Seconds of audio per configuration for the benchmark")

add_library(mw_sanitizers INTERFACE)

if(MW_TESTS_SANITIZE)
    if(MSVC)
        message(WARNING "MW_TESTS_SANITIZE under MSVC enables ASan only (no UBSan).")
        target_compile_options(mw_sanitizers INTERFACE /fsanitize=address)
    else()
        target_compile_options(mw_sanitizers INTERFACE
            -fsanitize=address,undefined -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
        target_link_options(mw_sanitizers INTERFACE -fsanitize=address,undefined)
    endif()
endif()

add_library(microwave_snd STATIC
    "${MW_SHARED_SRC}/snd.c"
    "${MW_SHARED_SRC}/snd_synth.c")

# The benchmark gets a separately compiled mixer so it stays genuinely
# unsanitized even when the correctness libraries are instrumented.
add_library(microwave_snd_bench STATIC
    "${MW_SHARED_SRC}/snd.c"
    "${MW_SHARED_SRC}/snd_synth.c")
target_include_directories(microwave_snd_bench PUBLIC "${MW_SHARED_SRC}")

add_library(microwave_seq STATIC
    "${MW_SHARED_SRC}/snd_seq.c"
    "${MW_SHARED_SRC}/mw_music_demo.c")
target_link_libraries(microwave_seq PUBLIC microwave_snd)

add_library(microwave_pack STATIC "${MW_SHARED_SRC}/snd_pack.c")
target_link_libraries(microwave_pack PUBLIC microwave_snd)

foreach(target microwave_snd microwave_seq microwave_pack)
    target_include_directories(${target} PUBLIC "${MW_SHARED_SRC}")
    # Instrument the libraries themselves, not only the test executables.
    # Otherwise the CI job name overstates its coverage.
    target_link_libraries(${target} PRIVATE mw_sanitizers)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(MW_TESTS_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion
            -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef)
        if(MW_TESTS_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endforeach()

if(MW_TESTS_U8)
    target_compile_definitions(microwave_snd PUBLIC SND_SAMPLE_U8=1)
    target_compile_definitions(microwave_snd_bench PUBLIC SND_SAMPLE_U8=1)
endif()
if(MW_TESTS_NARROW)
    target_compile_definitions(microwave_snd PUBLIC SND_WIDE_ACCUM=0)
    target_compile_definitions(microwave_snd_bench PUBLIC SND_WIDE_ACCUM=0)
endif()

add_library(microwave_core INTERFACE)
target_link_libraries(microwave_core INTERFACE microwave_seq microwave_pack)

function(mw_add_test_binary name source)
    add_executable(${name} "${source}")
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(${name} PRIVATE microwave_core mw_sanitizers)
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endif()
endfunction()

mw_add_test_binary(mw_test_unit mw_test_unit.c)
mw_add_test_binary(mw_test_fuzz mw_test_fuzz.c)

# The benchmark measures the mixer, so it is always built optimised and never
# linked against sanitizers, whatever the build type is.
add_executable(mw_test_bench mw_test_bench.c)
target_include_directories(mw_test_bench PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mw_test_bench PRIVATE microwave_snd_bench)
if(NOT MSVC)
    target_compile_options(mw_test_bench PRIVATE -O2 -Wall -Wextra)
endif()

enable_testing()

add_test(NAME unit COMMAND mw_test_unit)
set_tests_properties(unit PROPERTIES TIMEOUT 120 LABELS "unit")

# Several seeds, so a regression that only shows up for particular geometry has
# somewhere to surface. Each is deterministic and reproducible from its seed.
foreach(seed 0x5EED 0xC0FFEE 0xBADF00D 0x1337)
    add_test(NAME fuzz_${seed} COMMAND mw_test_fuzz ${MW_FUZZ_ITERATIONS} ${seed})
    set_tests_properties(fuzz_${seed} PROPERTIES TIMEOUT 300 LABELS "fuzz")
endforeach()

message(STATUS "MicroWave tests: build=${CMAKE_BUILD_TYPE} "
               "sanitize=${MW_TESTS_SANITIZE} u8=${MW_TESTS_U8} "
               "narrow=${MW_TESTS_NARROW}")
