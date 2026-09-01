@echo off
rem ===========================================================================
rem MicroWave - single entry point for every build and run task.
rem
rem Deliberately mirrors MicroRender's mr.bat: variants are arguments, not
rem separate scripts, and the complete argument list is forwarded to workers.
rem
rem   .\mw.bat build assets
rem   .\mw.bat build dos
rem   .\mw.bat build pico [max98357a|pcm5102a|ns4168|all] [settings]
rem   .\mw.bat build pico <preset> vscode [settings]
rem   .\mw.bat build raylib [CMake -D options]
rem   .\mw.bat build headless [CMake -D options]
rem   .\mw.bat build tests
rem   .\mw.bat build all
rem
rem   .\mw.bat run dos [seconds]
rem   .\mw.bat run raylib [args...]
rem   .\mw.bat run headless [args...]
rem   .\mw.bat run pico [preset] [swd|picotool|manual] [build settings]
rem
rem   .\mw.bat test [default|u8|narrow|all]
rem   .\mw.bat bench [seconds]
rem   .\mw.bat clean
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "MW_ROOT=%CD%"

if not exist "%MW_ROOT%\shared\src\snd.c" goto not_a_repo
if not exist "%MW_ROOT%\shared\tools\mw_pack.py" goto not_a_repo

set "CMD=%~1"
if "%CMD%"=="" set "CMD=help"

if /i "%CMD%"=="build" goto do_build
if /i "%CMD%"=="run"   goto do_run
if /i "%CMD%"=="test"  goto do_test
if /i "%CMD%"=="bench" goto do_bench
if /i "%CMD%"=="clean" goto do_clean
if /i "%CMD%"=="help"  goto do_help
if /i "%CMD%"=="-h"    goto do_help
if /i "%CMD%"=="--help" goto do_help

echo ERROR: unknown command "%CMD%".
echo.
goto do_help

:do_build
call "%MW_ROOT%\scripts\mw_build.bat" %*
exit /b %ERRORLEVEL%

:do_run
call "%MW_ROOT%\scripts\mw_run.bat" %*
exit /b %ERRORLEVEL%

:do_test
shift /1
set "VARIANT=%~1"
if "%VARIANT%"=="" set "VARIANT=default"
if /i "%VARIANT%"=="all" (
    call :run_one_test default || exit /b 1
    call :run_one_test u8 || exit /b 1
    call :run_one_test narrow || exit /b 1
    echo.
    echo All test variants passed.
    exit /b 0
)
call :run_one_test "%VARIANT%"
exit /b %ERRORLEVEL%

:run_one_test
setlocal
set "V=%~1"
set "PRESET=tests"
set "DIR=build-tests"
if /i "%V%"=="u8"     (set "PRESET=u8"& set "DIR=build-tests-u8")
if /i "%V%"=="narrow" (set "PRESET=narrow"& set "DIR=build-tests-narrow")
if /i not "%V%"=="default" if /i not "%V%"=="u8" if /i not "%V%"=="narrow" (
    echo ERROR: unknown test variant "%V%".
    endlocal & exit /b 1
)
echo.
echo === tests: %V% ===
pushd "%MW_ROOT%\tests"
cmake --preset %PRESET% || (popd & endlocal & exit /b 1)
cmake --build --preset %PRESET% || (popd & endlocal & exit /b 1)
popd
ctest --test-dir "%MW_ROOT%\%DIR%" --output-on-failure || (endlocal & exit /b 1)
endlocal & exit /b 0

:do_bench
shift /1
set "SECS=%~1"
if "%SECS%"=="" set "SECS=5"
pushd "%MW_ROOT%\tests"
cmake --preset bench || (popd & exit /b 1)
cmake --build --preset bench || (popd & exit /b 1)
popd
"%MW_ROOT%\build-bench\mw_test_bench" %SECS%
exit /b %ERRORLEVEL%

:do_clean
call "%MW_ROOT%\scripts\mw_clean.bat"
exit /b %ERRORLEVEL%

:not_a_repo
echo ERROR: this does not look like the MicroWave repository root.
echo        Looking in: %MW_ROOT%
exit /b 1

:do_help
echo MicroWave build and run driver.
echo.
echo   .\mw.bat build assets ^| dos ^| pico [preset] [settings] ^| raylib ^| headless ^| tests ^| all
echo   .\mw.bat run dos ^| raylib ^| headless ^| pico [preset] [flash-method]
echo   .\mw.bat test [default^|u8^|narrow^|all]
echo   .\mw.bat bench [seconds]
echo   .\mw.bat clean
echo.
echo Pico presets:
echo   max98357a    MAX98357A I2S Class-D amplifier
echo   pcm5102a     PCM5102A stereo I2S DAC, three-wire PLL mode
echo   ns4168       NS4168 I2S Class-D amplifier
echo   all          build all three presets
echo.
echo Pico settings:
echo   rate=N       sample rate; all presets default to 32000 Hz
echo   block=N      mixer/DMA frames per block, default 256
echo   bclk=N       BCLK GPIO, default 10
echo   lrclk=N      LRCLK GPIO, default 11 and must equal BCLK+1
echo   data=N       DATA/DIN/SDATA GPIO, default 12
echo   sys=N        optional system clock in kHz; 0 leaves board default
echo   pio=0/1 sm=0..3 dma=N serial=ON/OFF
echo   MW_...=VALUE passes an advanced CMake cache variable through directly
echo.
echo Pico runner:
echo   .\mw.bat run pico max98357a swd
echo   .\mw.bat run pico pcm5102a picotool
echo   .\mw.bat run pico ns4168 manual
echo.
echo Wiring is always three-wire standard I2S:
echo   GP10 BCLK, GP11 LRCLK, GP12 DATA by default.
exit /b 0
