@echo off
rem MicroWave build driver. Invoked through mw.bat; see there for usage.
setlocal EnableExtensions EnableDelayedExpansion
if "%MW_ROOT%"=="" set "MW_ROOT=%~dp0.."
cd /d "%MW_ROOT%"

if /i "%~1"=="build" shift /1
set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=all"

if /i "%WHAT%"=="assets"   goto b_assets
if /i "%WHAT%"=="tests"    goto b_tests
if /i "%WHAT%"=="headless" goto b_headless
if /i "%WHAT%"=="raylib"   goto b_raylib
if /i "%WHAT%"=="dos"      goto b_dos
if /i "%WHAT%"=="pico"     goto b_pico
if /i "%WHAT%"=="all"      goto b_all

echo ERROR: unknown build target "%WHAT%".
exit /b 1

:b_assets
echo === assets ===
call "%MW_ROOT%\scripts\mw_tools.bat" python
if errorlevel 1 exit /b 1
if not exist "shared\assets\audio\pickup.wav" (
    echo   no sample WAVs in shared\assets\audio; nothing to pack.
    echo   The demo song is synthesized and needs no assets, so this is fine.
    exit /b 0
)
"%MW_PYTHON%" "shared\tools\mw_pack.py" --out "GAME.MWP" --format u8 --wav pickup=shared\assets\audio\pickup.wav --wav blip=shared\assets\audio\blip.wav --wav sweep=shared\assets\audio\sweep.wav
exit /b %ERRORLEVEL%

:b_tests
echo === host tests ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 exit /b 1
pushd tests
"%MW_CMAKE%" --preset tests || (popd & exit /b 1)
"%MW_CMAKE%" --build --preset tests || (popd & exit /b 1)
popd
exit /b 0

:b_headless
shift /1
set "HOST_ARGS="
:headless_args
if "%~1"=="" goto headless_build
set "HOST_ARGS=!HOST_ARGS! "%~1""
shift /1
goto headless_args
:headless_build
echo === headless frontend ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 exit /b 1
"%MW_CMAKE%" -S microwave_raylib -B build-headless -DMW_RAYLIB_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release !HOST_ARGS! || exit /b 1
"%MW_CMAKE%" --build build-headless --config Release || exit /b 1
echo   built build-headless\Release\microwave_raylib.exe
exit /b 0

:b_raylib
shift /1
set "HOST_ARGS="
:raylib_args
if "%~1"=="" goto raylib_build
set "HOST_ARGS=!HOST_ARGS! "%~1""
shift /1
goto raylib_args
:raylib_build
echo === raylib frontend ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 exit /b 1
"%MW_CMAKE%" -S microwave_raylib -B build-raylib -DCMAKE_BUILD_TYPE=Release !HOST_ARGS! || exit /b 1
"%MW_CMAKE%" --build build-raylib --config Release || exit /b 1
echo   built build-raylib\Release\microwave_raylib.exe
exit /b 0

:b_dos
echo === 16-bit DOS frontend ===
call "%MW_ROOT%\scripts\mw_tools.bat" watcom
if errorlevel 1 exit /b 1
set "PATH=%MW_WATCOM%\binnt64;%MW_WATCOM%\binnt;%PATH%"
set "INCLUDE=%MW_WATCOM%\h;%MW_WATCOM%\h\nt"
if not exist "build-dos" mkdir "build-dos"
wcl -ml -0 -ox -bt=dos -fe=build-dos\MWDEMO.EXE -i=shared\src microwave_dos\dos\main.c shared\src\snd.c shared\src\snd_synth.c shared\src\snd_seq.c shared\src\mw_music_demo.c || exit /b 1
echo   built build-dos\MWDEMO.EXE
exit /b 0

:b_pico
echo === RP2350 I2S firmware ===
call "%MW_ROOT%\scripts\mw_tools.bat" cmake
if errorlevel 1 exit /b 1
call "%MW_ROOT%\microwave\pico_env_auto.bat"
if errorlevel 1 exit /b 1

for %%D in ("%NINJA_EXE%") do set "PATH=%%~dpD;%PICO_TOOLCHAIN_PATH%\bin;%PATH%"
set "PICO_SOURCE=%MW_ROOT%\microwave"
set "PRESET=%~2"
if "%PRESET%"=="" set "PRESET=max98357a"
set "PICO_VSCODE=0"
set "EXTRA_FLAGS="

shift /1
if not "%~1"=="" shift /1

:pico_opts
if "%~1"=="" goto pico_opts_done
if /i "%~1"=="vscode" (
    set "PICO_VSCODE=1"
    shift /1
    goto pico_opts
)

set "MW_OPT=%~1"
set "MW_KEY=%~1"
set "MW_VALUE=%~2"
set "MW_OPT_ARGC=2"
for /f "tokens=1,* delims==" %%A in ("!MW_OPT!") do (
    if not "%%B"=="" (
        set "MW_KEY=%%A"
        set "MW_VALUE=%%B"
        set "MW_OPT_ARGC=1"
    )
)
if "!MW_VALUE!"=="" (
    echo ERROR: Pico option "!MW_KEY!" has an empty value.
    echo Use either !MW_KEY!=VALUE or "!MW_KEY!=VALUE".
    exit /b 1
)

set "MW_OPT_HANDLED=0"
if /i "!MW_KEY:~0,3!"=="MW_" (
    set "EXTRA_FLAGS=!EXTRA_FLAGS! -D!MW_KEY!=!MW_VALUE!"
    set "MW_OPT_HANDLED=1"
)
if /i "!MW_KEY!"=="rate"   (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_PICO_RATE=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="block"  (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_PICO_BLOCK=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="sys"    (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_PICO_SYS_KHZ=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="bclk"   (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_PIN_BCLK=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="lrclk"  (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_PIN_LRCLK=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="data"   (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_PIN_DATA=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="din"    (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_PIN_DATA=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="sdata"  (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_PIN_DATA=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="pio"    (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_PIO=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="sm"     (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_SM=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="dma"    (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_I2S_DMA=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if /i "!MW_KEY!"=="serial" (set "EXTRA_FLAGS=!EXTRA_FLAGS! -DMW_PICO_SERIAL=!MW_VALUE!"& set "MW_OPT_HANDLED=1")
if "!MW_OPT_HANDLED!"=="0" (
    echo ERROR: unknown Pico build option "!MW_KEY!".
    exit /b 1
)
if "!MW_OPT_ARGC!"=="2" shift /1
shift /1
goto pico_opts

:pico_opts_done
if /i "%PRESET%"=="all" goto b_pico_all
call :pico_preset_flags "%PRESET%"
if errorlevel 1 exit /b 1
if "%PICO_VSCODE%"=="1" goto b_pico_vscode
call :pico_build_preset "%PRESET%"
exit /b %ERRORLEVEL%

:b_pico_all
if "%PICO_VSCODE%"=="1" (
    echo ERROR: vscode needs one Pico preset, not all.
    exit /b 1
)
for %%P in (max98357a pcm5102a ns4168) do (
    call :pico_preset_flags "%%P"
    if errorlevel 1 exit /b 1
    call :pico_build_preset "%%P"
    if errorlevel 1 exit /b 1
)
echo [pico] all I2S device presets built successfully
exit /b 0

:pico_preset_flags
set "PICO_ONE_PRESET=%~1"
set "PRESET_FLAGS="
if /i "%PICO_ONE_PRESET%"=="max98357a" set "PRESET_FLAGS=-DMW_PICO_DEVICE=MAX98357A -DMW_PICO_RATE=32000"
if /i "%PICO_ONE_PRESET%"=="pcm5102a"  set "PRESET_FLAGS=-DMW_PICO_DEVICE=PCM5102A -DMW_PICO_RATE=32000"
if /i "%PICO_ONE_PRESET%"=="ns4168"    set "PRESET_FLAGS=-DMW_PICO_DEVICE=NS4168 -DMW_PICO_RATE=32000"
if not defined PRESET_FLAGS (
    echo ERROR: unknown Pico preset "%PICO_ONE_PRESET%".
    echo        Use max98357a, pcm5102a, ns4168, or all.
    exit /b 1
)
exit /b 0

:pico_build_preset
set "PICO_ONE_PRESET=%~1"
set "PICO_BUILD_DIR=%PICO_SOURCE%\build-%PICO_ONE_PRESET%"
call :pico_prepare_build_dir "%PICO_BUILD_DIR%" "%PICO_ONE_PRESET% PICO_BOARD=%PICO_BOARD% %PRESET_FLAGS% %EXTRA_FLAGS%"
if errorlevel 1 exit /b 1

echo [pico] configuring "%PICO_ONE_PRESET%" %EXTRA_FLAGS% ...
pushd "%PICO_SOURCE%" >nul
cmake --preset "%PICO_ONE_PRESET%" -DPICO_BOARD=%PICO_BOARD% -DCMAKE_MAKE_PROGRAM:FILEPATH="%NINJA_EXE%" %EXTRA_FLAGS%
if errorlevel 1 (set "RC=!ERRORLEVEL!"& popd >nul& exit /b !RC!)
cmake --build --preset "%PICO_ONE_PRESET%" --parallel
if errorlevel 1 (set "RC=!ERRORLEVEL!"& popd >nul& exit /b !RC!)
popd >nul
echo [pico] built %PICO_BUILD_DIR%\microwave.uf2
exit /b 0

:b_pico_vscode
set "PICO_BUILD_DIR=%PICO_SOURCE%\build"
call :pico_prepare_build_dir "%PICO_BUILD_DIR%" "%PRESET% vscode PICO_BOARD=%PICO_BOARD% %PRESET_FLAGS% %EXTRA_FLAGS%"
if errorlevel 1 exit /b 1
echo [pico] configuring "%PRESET%" into microwave\build for VS Code ...
cmake -S "%PICO_SOURCE%" -B "%PICO_BUILD_DIR%" -G Ninja -DPICO_BOARD=%PICO_BOARD% -DCMAKE_MAKE_PROGRAM:FILEPATH="%NINJA_EXE%" %PRESET_FLAGS% %EXTRA_FLAGS%
if errorlevel 1 exit /b 1
cmake --build "%PICO_BUILD_DIR%" --parallel
if errorlevel 1 exit /b 1
echo [pico] microwave\build is now the "%PRESET%" build.
exit /b 0

:pico_prepare_build_dir
set "PICO_CHECK_DIR=%~1"
set "PICO_WANT_FLAGS=%~2"
set "PICO_STAMP=%PICO_CHECK_DIR%\.mw_build_flags"
set "PICO_OLD_FLAGS="
if exist "%PICO_STAMP%" set /p PICO_OLD_FLAGS=<"%PICO_STAMP%"
if exist "%PICO_CHECK_DIR%" if not "!PICO_OLD_FLAGS!"=="!PICO_WANT_FLAGS!" (
    echo [pico] configuration changed; clearing stale cache in %PICO_CHECK_DIR%
    rmdir /s /q "%PICO_CHECK_DIR%"
)
if not exist "%PICO_CHECK_DIR%" mkdir "%PICO_CHECK_DIR%"
>"%PICO_STAMP%" echo %PICO_WANT_FLAGS%
exit /b 0

:b_all
echo === building everything the local toolchain supports ===
call "%~f0" tests || exit /b 1
call "%~f0" headless || exit /b 1
call "%MW_ROOT%\scripts\mw_tools.bat" watcom >nul 2>&1
if not errorlevel 1 (call "%~f0" dos || exit /b 1) else echo   skipping dos: Open Watcom not found
call "%MW_ROOT%\microwave\pico_env_auto.bat" >nul 2>&1
if not errorlevel 1 (call "%~f0" pico max98357a || exit /b 1) else echo   skipping pico: Pico SDK/toolchain not found
echo.
echo   raylib is not built by "all" because it may require a submodule checkout.
exit /b 0
