# Building MicroWave

Every task goes through one entry point, exactly as MicroRender routes
everything through `mr.bat`:

```
.\mw.bat build <assets|dos|pico|raylib|headless|tests|all>
.\mw.bat run   <dos|raylib|headless> [args...]
.\mw.bat test  [default|u8|narrow|all]
.\mw.bat bench [seconds]
.\mw.bat clean
```

Variants are arguments, not separate files.

On Linux and macOS, drive CMake directly; nothing in the library or the tests
needs Windows.

## Host tests

The only build that requires no hardware, no emulator and no SDK.

```
cd tests
cmake --preset tests
cmake --build --preset tests
ctest --test-dir ../build-tests --output-on-failure
```

Presets are invoked from inside `tests/`, since a preset implies its own source
directory.

| preset | what it builds |
| --- | --- |
| `tests` | Debug, ASan + UBSan, S16 samples, wide accumulator |
| `u8` | the legacy 8-bit sample format |
| `narrow` | the DOS-style saturating mix path on a 32-bit host |
| `bench` | Release, no sanitizers — the benchmark is meaningless under ASan |

`.\mw.bat test all` runs the first three in sequence.

Useful cache variables:

| variable | default | effect |
| --- | --- | --- |
| `MW_TESTS_SANITIZE` | ON in Debug | ASan + UBSan on the libraries, not just the executables |
| `MW_TESTS_WERROR` | ON | warnings are errors in the shared mixer |
| `MW_FUZZ_ITERATIONS` | 200 | iterations per fuzz seed under ctest |
| `MW_TESTS_U8` | OFF | build `snd_sample_t` as unsigned 8-bit |
| `MW_TESTS_NARROW` | OFF | force `SND_WIDE_ACCUM=0` |

The sanitizers are applied to the *libraries* as well as the test binaries. A
job that instruments only the executable overstates what it covers.

## Using the library in your own project

```cmake
add_subdirectory(path/to/microwave/shared)
target_link_libraries(yourgame PRIVATE microwave::snd)
```

Three targets, deliberately separate:

| target | contents | pulls in |
| --- | --- | --- |
| `microwave::snd` | `snd.c`, `snd_synth.c` | nothing; no stdio, no malloc |
| `microwave::seq` | `snd_seq.c` | `microwave::snd` |
| `microwave::pack` | `snd_pack.c` | `microwave::snd`, stdio |

Enable the optional ones with `-DMW_BUILD_SEQUENCER=ON` and
`-DMW_BUILD_PACK=ON`.

If you are not using CMake, the library is six files and has no build
requirements beyond a C99 compiler. Add `shared/src` to your include path and
compile `snd.c` and `snd_synth.c`.

## Toolchain locations

`scripts/mw_tools.bat` resolves every external tool, so no other script
hardcodes a path. It is the direct counterpart of MicroRender's
`mr_tools.bat`.

| variable | needed for | typical value | notes |
| --- | --- | --- | --- |
| `WATCOM` | DOS | `C:\WATCOM` | must contain `binnt64\wcc.exe`, `binnt\`, or `binw\` |
| `PICO_SDK_PATH` | RP2350 | `C:\pico\pico-sdk` | must contain `pico_sdk_init.cmake` |
| `DOSBOX_EXE` | `run dos` | full path to the `.exe` | optional; PATH and the usual install folders are searched |

Set them for one session:

```bat
set WATCOM=C:\WATCOM
set PICO_SDK_PATH=C:\pico\pico-sdk
.\mw.bat build all
```

Or permanently with `setx WATCOM "C:\WATCOM"` — note that `setx` does not
affect the window you run it in, so open a new terminal afterwards. The GUI
equivalent is Win+R, `sysdm.cpl`, Advanced, Environment Variables.

`cmake` and `python` are found on `PATH`. Python is only needed by the asset
packer; the demo song is synthesized and needs no assets.

**There is no raylib environment variable.** See below.

## Desktop (Raylib)

Raylib is resolved by `microwave_raylib/CMakeLists.txt`, in the same order and
for the same reasons as MicroRender's:

1. `-DMW_RAYLIB_PATH=...`, an explicit source checkout
2. a pinned `third_party/raylib` submodule
3. `find_package(raylib CONFIG)`
4. a bare `raylib.h` / `libraylib` pair the toolchain can already see

If you want the pinned-submodule arrangement MicroRender uses:

```
git submodule add https://github.com/raysan5/raylib.git third_party/raylib
git submodule update --init --recursive
```

Then:

```
.\mw.bat build raylib
.\mw.bat run raylib --pack GAME.MWP --entry pickup
```

Extra CMake options pass straight through:

```
.\mw.bat build raylib -DMW_RAYLIB_PATH=C:/src/raylib
.\mw.bat build raylib -DMW_RAYLIB_BLOCK=97 -DMW_RAYLIB_CHANNELS=2
```

`MW_RAYLIB_BLOCK` is genuinely useful: the mixer is block-size independent by
construction, so building the frontend at an awkward block size and comparing
the rendered WAV is a real end-to-end check of that property.

The headless build is the same `main.c` and the same `CMakeLists.txt` with the
device calls compiled out, and needs no raylib at all:

```
.\mw.bat build headless
.\mw.bat run headless --seconds 10 --wav demo.wav
```

That is what CI builds, and it is why the frontend is worth testing at all.

Options accepted by both builds:

| option | meaning |
| --- | --- |
| `--seconds N` | how long to play; default is one pass of the song |
| `--pack PATH` | load effects from an `MWP1` **or** `MRP1` pack |
| `--entry NAME` | which pack entry to use as the effect |
| `--wav PATH` | also write the mix to a RIFF WAV |

## RP2350

Requires `PICO_SDK_PATH` and `pico_sdk_import.cmake` in `microwave/`.

```
.\mw.bat build pico
```

Produces `build-pico/microwave.uf2`. Default output is 8-bit PWM on GPIO 18,
one RC filter away from a speaker. `snd_pack.c` is deliberately excluded from
this build: it needs stdio, and the synthesized demo needs no files.

## DOS

Requires Open Watcom; set `WATCOM` to its install root. `mw_tools.bat` checks
for `wcc.exe` under `binnt64\`, `binnt\` and `binw\` and says which it looked
in if it fails.

```
.\mw.bat build dos
.\mw.bat run dos 10
```

Built large-model (`-ml -0`). The mixer's own working set is small — a
256-frame mono S16 block is 512 bytes — but sample data is not, and far
pointers are what let a clip live outside the default segment. Keep individual
clips under 64 KiB; `snd_clip_validate()` will tell you if one is not
addressable.

`DOSBOX_EXE` is optional — `mw_tools.bat` searches `PATH` and the usual
DOSBox and DOSBox-X install folders first. `MW_DOSBOX_CYCLES`
defaults to `max`; pin it before quoting any performance figure:

| cycles | approximates |
| --- | --- |
| `fixed 3000` | 386DX/33 |
| `fixed 12000` | 486DX2/66 |
| `fixed 30000` | Pentium 100 |

A mixer that keeps up at `max` has told you about your host CPU and nothing
about a 386.

## Assets

```
.\mw.bat build assets
```

runs `shared/tools/mw_pack.py` over `shared/assets/audio` and writes
`GAME.MWP`. Directly:

```
python shared/tools/mw_pack.py --out GAME.MWP --format u8 \
    --wav pickup=shared/assets/audio/pickup.wav

python shared/tools/mw_pack.py --inspect GAME.MRP
```

`--format` is `u8`, `s16` or `adpcm`. `u8` is the default because it is the one
format MicroRender's reader also understands, so a `u8` pack is readable by
both libraries. Input WAVs must be mono PCM, 8- or 16-bit.

`--inspect` works on either container and reports which entries MicroWave can
actually use, which is the quick way to check whether an existing `GAME.MRP`
carries audio.

## Cross-compiler notes

- **Open Watcom** uses a 16-bit `int` even in large model. All frame addressing
  in the mixer is `long` for that reason, and `snd_clip_validate()` refuses
  clips whose frame count cannot be expressed.
- **MSVC** builds the tests without UBSan; `MW_TESTS_SANITIZE=ON` enables ASan
  only, and CI runs the MSVC job unsanitized.
- If your compiler needs explicit far annotations, define `SND_PTR` before
  including the headers, e.g. `-DSND_PTR=__far`.
