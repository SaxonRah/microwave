@echo off
rem ===========================================================================
rem  MicroWave - single entry point for every build and run task.
rem
rem  Deliberately the same shape as MicroRender's mr.bat, and for the same
rem  reason: variants are arguments, not separate files. If you know one, you
rem  know the other.
rem
rem    .\mw.bat build assets           regenerate GAME.MWP from shared\assets
rem    .\mw.bat build dos              16-bit DOS Sound Blaster frontend
rem    .\mw.bat build pico [key=value ...]
rem                              RP2350 firmware
rem    .\mw.bat build raylib [-Dkey=value ...]
rem                              desktop frontend
rem    .\mw.bat build headless         desktop frontend with no audio device,
rem                              which is what CI builds
rem    .\mw.bat build tests            host test binaries
rem    .\mw.bat build all              everything the local toolchain supports
rem
rem    .\mw.bat run dos [seconds]      Sound Blaster demo in DOSBox
rem    .\mw.bat run raylib [args...]   desktop demo
rem    .\mw.bat run headless [args...] render to a WAV with no device
rem    .\mw.bat test [variant]         host unit + fuzz suite under ASan/UBSan
rem                              variant: default | u8 | narrow | all
rem    .\mw.bat bench [seconds]        host benchmark, unsanitized
rem
rem    .\mw.bat clean                  remove all build output
rem    .\mw.bat help                   this text
rem
rem  Environment overrides (all resolved by scripts\mw_tools.bat):
rem    WATCOM              Open Watcom install root, e.g. C:\WATCOM
rem    PICO_SDK_PATH       Pico SDK root, e.g. C:\pico\pico-sdk
rem    DOSBOX_EXE          full path to dosbox-x.exe or dosbox.exe. Optional:
rem                        PATH and the usual install folders are searched.
rem
rem  Raylib has no environment variable on purpose. It is resolved by
rem  microwave_raylib\CMakeLists.txt, which prefers third_party\raylib, then an
rem  installed package. Override with:
rem    .\mw.bat build raylib -DMW_RAYLIB_PATH=C:/path/to/raylib
rem    MW_DOSBOX_CYCLES    DOSBox cycles; default "max". Period-accurate values:
rem                          fixed 3000   ~386DX/33
rem                          fixed 12000  ~486DX2/66
rem                        A mixer that keeps up at "max" tells you nothing
rem                        about a 386. Pin this before quoting a realtime
rem                        ratio anywhere.
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "MW_ROOT=%CD%"

rem Sanity check: this must run from the repository root.
if not exist "%MW_ROOT%\shared\src\snd.c" goto not_a_repo
if not exist "%MW_ROOT%\shared\tools\mw_pack.py" goto not_a_repo

set "CMD=%~1"
if "%CMD%"=="" set "CMD=help"
shift /1

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

rem ---------------------------------------------------------------------------
:do_build
set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=all"
shift /1
call "%MW_ROOT%\scripts\mw_build.bat" "%WHAT%" %1 %2 %3 %4 %5 %6
goto end

rem ---------------------------------------------------------------------------
:do_run
set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=raylib"
shift /1
call "%MW_ROOT%\scripts\mw_run.bat" "%WHAT%" %1 %2 %3 %4 %5 %6
goto end

rem ---------------------------------------------------------------------------
:do_test
set "VARIANT=%~1"
if "%VARIANT%"=="" set "VARIANT=default"

if /i "%VARIANT%"=="all" (
    call :run_one_test default  || goto fail
    call :run_one_test u8       || goto fail
    call :run_one_test narrow   || goto fail
    echo.
    echo All test variants passed.
    goto end
)
call :run_one_test %VARIANT% || goto fail
goto end

:run_one_test
setlocal
set "V=%~1"
set "PRESET=tests"
set "DIR=build-tests"
if /i "%V%"=="u8"     ( set "PRESET=u8"     & set "DIR=build-tests-u8" )
if /i "%V%"=="narrow" ( set "PRESET=narrow" & set "DIR=build-tests-narrow" )

echo.
echo === tests: %V% ===
pushd "%MW_ROOT%\tests"
cmake --preset %PRESET% || (popd & endlocal & exit /b 1)
cmake --build --preset %PRESET% || (popd & endlocal & exit /b 1)
popd
ctest --test-dir "%MW_ROOT%\%DIR%" --output-on-failure || (endlocal & exit /b 1)
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
:do_bench
set "SECS=%~1"
if "%SECS%"=="" set "SECS=5"
pushd "%MW_ROOT%\tests"
cmake --preset bench || (popd & goto fail)
cmake --build --preset bench || (popd & goto fail)
popd
"%MW_ROOT%\build-bench\mw_test_bench" %SECS%
goto end

rem ---------------------------------------------------------------------------
:do_clean
call "%MW_ROOT%\scripts\mw_clean.bat"
goto end

rem ---------------------------------------------------------------------------
:do_help
echo.
echo MicroWave build commands:
echo.
echo   mw.bat build ^<assets^|dos^|pico^|raylib^|headless^|tests^|all^>
echo   mw.bat run   ^<dos^|raylib^|headless^> [args...]
echo   mw.bat test  [default^|u8^|narrow^|all]
echo   mw.bat bench [seconds]
echo   mw.bat clean
echo.
echo See the comment block at the top of this file for the full list.
goto end

rem ---------------------------------------------------------------------------
:not_a_repo
echo ERROR: mw.bat must be run from the MicroWave repository root.
echo        Expected to find shared\src\snd.c below "%MW_ROOT%".
exit /b 1

:fail
echo.
echo BUILD OR TEST FAILED.
endlocal
exit /b 1

:end
endlocal
exit /b 0
