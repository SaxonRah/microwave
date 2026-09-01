@echo off
rem Build dispatcher, called by mw.bat. Not intended to be run directly.
rem Toolchain locations come from mw_tools.bat; nothing here hardcodes a path.
setlocal EnableExtensions
set "MW_ROOT=%~dp0.."
pushd "%MW_ROOT%"

set "WHAT=%~1"
shift /1

if /i "%WHAT%"=="assets"   goto b_assets
if /i "%WHAT%"=="tests"    goto b_tests
if /i "%WHAT%"=="headless" goto b_headless
if /i "%WHAT%"=="raylib"   goto b_raylib
if /i "%WHAT%"=="dos"      goto b_dos
if /i "%WHAT%"=="pico"     goto b_pico
if /i "%WHAT%"=="all"      goto b_all

echo ERROR: unknown build target "%WHAT%".
goto fail

rem ---------------------------------------------------------------------------
:b_assets
echo === assets ===
call "%MW_ROOT%\scripts\mw_tools.bat" python
if errorlevel 1 goto fail
if not exist "shared\assets\audio\pickup.wav" (
    echo   no sample WAVs in shared\assets\audio; nothing to pack.
    echo   The demo song is synthesized and needs no assets, so this is fine.
    goto ok
)
"%MW_PYTHON%" "shared\tools\mw_pack.py" --out "GAME.MWP" --format u8 --wav pickup=shared\assets\audio\pickup.wav --wav blip=shared\assets\audio\blip.wav --wav sweep=shared\assets\audio\sweep.wav || goto fail
goto ok

rem ---------------------------------------------------------------------------
:b_tests
echo === host tests ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 goto fail
pushd tests
"%MW_CMAKE%" --preset tests || (popd & goto fail)
"%MW_CMAKE%" --build --preset tests || (popd & goto fail)
popd
goto ok

rem ---------------------------------------------------------------------------
rem The headless frontend is the same main.c and the same CMakeLists the audio
rem build uses, with MW_RAYLIB_HEADLESS=ON compiling out the device calls. It
rem needs no raylib at all, which is why CI can build it.
:b_headless
echo === headless frontend ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 goto fail
"%MW_CMAKE%" -S microwave_raylib -B build-headless -DMW_RAYLIB_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release %1 %2 %3 %4 || goto fail
"%MW_CMAKE%" --build build-headless --config Release || goto fail
echo   built build-headless\microwave_raylib.exe
goto ok

rem ---------------------------------------------------------------------------
rem Raylib itself is located by microwave_raylib\CMakeLists.txt, which prefers
rem the pinned submodule and falls back to an installed package. There is no
rem RAYLIB_PATH to set; pass -DMW_RAYLIB_PATH=... if you need an override.
:b_raylib
echo === raylib frontend ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 goto fail
"%MW_CMAKE%" -S microwave_raylib -B build-raylib -DCMAKE_BUILD_TYPE=Release %1 %2 %3 %4 || goto fail
"%MW_CMAKE%" --build build-raylib --config Release || goto fail
echo   built build-raylib\microwave_raylib.exe
goto ok

rem ---------------------------------------------------------------------------
:b_dos
echo === 16-bit DOS frontend ===
call "%MW_ROOT%\scripts\mw_tools.bat" watcom
if errorlevel 1 goto fail
set "PATH=%MW_WATCOM%\binnt64;%MW_WATCOM%\binnt;%PATH%"
set "INCLUDE=%MW_WATCOM%\h;%MW_WATCOM%\h\nt"
if not exist "build-dos" mkdir "build-dos"

rem Large model. The mixer's own working set is small -- a 256-frame mono S16
rem block is 512 bytes -- but sample data is not, and far pointers are what let
rem a clip live outside the default segment.
wcl -ml -0 -ox -bt=dos -fe=build-dos\MWDEMO.EXE -i=shared\src microwave_dos\dos\main.c shared\src\snd.c shared\src\snd_synth.c shared\src\snd_seq.c shared\src\mw_music_demo.c || goto fail
echo   built build-dos\MWDEMO.EXE
goto ok

rem ---------------------------------------------------------------------------
:b_pico
echo === RP2350 firmware ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 goto fail
if "%PICO_SDK_PATH%"=="" (
    echo ERROR: PICO_SDK_PATH is not set. Point it at your Pico SDK root.
    echo        set PICO_SDK_PATH=C:\pico\pico-sdk
    goto fail
)
if not exist "%PICO_SDK_PATH%\pico_sdk_init.cmake" (
    echo ERROR: pico_sdk_init.cmake not found under "%PICO_SDK_PATH%".
    goto fail
)
if not exist "microwave\pico_sdk_import.cmake" (
    echo ERROR: microwave\pico_sdk_import.cmake is missing.
    echo        Copy it from "%PICO_SDK_PATH%\external\pico_sdk_import.cmake".
    goto fail
)
"%MW_CMAKE%" -S microwave -B build-pico -DPICO_BOARD=pico2 || goto fail
"%MW_CMAKE%" --build build-pico || goto fail
echo   built build-pico\microwave.uf2
goto ok

rem ---------------------------------------------------------------------------
rem "all" builds what the local toolchain actually supports and reports what it
rem skipped, rather than failing because you do not happen to own an Open
rem Watcom install today.
:b_all
echo === building everything the local toolchain supports ===
call "%~f0" tests    || goto fail
call "%~f0" headless || goto fail
call "%MW_ROOT%\scripts\mw_tools.bat" watcom >nul 2>&1
if not errorlevel 1 ( call "%~f0" dos || goto fail ) else echo   skipping dos: Open Watcom not found
if not "%PICO_SDK_PATH%"=="" ( call "%~f0" pico || goto fail ) else echo   skipping pico: PICO_SDK_PATH not set
echo.
echo   raylib is not built by "all" because it may need a submodule checkout.
echo   Run: git submodule update --init --recursive ^&^& mw.bat build raylib
goto ok

:fail
popd
endlocal
exit /b 1

:ok
popd
endlocal
exit /b 0
