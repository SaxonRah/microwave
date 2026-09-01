@echo off
rem ===========================================================================
rem MicroWave run dispatcher, called by mw.bat.
rem
rem Host CMake generators may be single-config (executable directly in the
rem build directory) or multi-config, as with Visual Studio (Release\ beneath
rem the build directory). Resolve both forms rather than assuming one.
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "MW_ROOT=%~dp0.."
pushd "%MW_ROOT%"
if errorlevel 1 goto fail

set "WHAT=%~1"
shift /1

if /i "%WHAT%"=="headless" goto r_headless
if /i "%WHAT%"=="raylib"   goto r_raylib
if /i "%WHAT%"=="dos"      goto r_dos

echo ERROR: unknown run target "%WHAT%".
goto fail


rem ---------------------------------------------------------------------------
rem Desktop/headless host frontends
rem ---------------------------------------------------------------------------

:r_headless
set "BUILD_TARGET=headless"
set "BUILD_DIR=build-headless"
goto r_host

:r_raylib
set "BUILD_TARGET=raylib"
set "BUILD_DIR=build-raylib"
goto r_host


:r_host
call :resolve_host_exe

if not defined HOST_EXE (
    echo %BUILD_TARGET% frontend is not built; building it now...
    call "%MW_ROOT%\scripts\mw_build.bat" "%BUILD_TARGET%"
    if errorlevel 1 goto fail

    call :resolve_host_exe
)

if not defined HOST_EXE (
    echo ERROR: %BUILD_TARGET% frontend build completed but the executable
    echo        could not be found in either:
    echo        %MW_ROOT%\%BUILD_DIR%\microwave_raylib.exe
    echo        %MW_ROOT%\%BUILD_DIR%\Release\microwave_raylib.exe
    goto fail
)

rem Preserve every frontend argument rather than arbitrarily stopping at six.
set "MW_FORWARD_ARGS="

:r_host_args
if "%~1"=="" goto r_host_launch
set "MW_FORWARD_ARGS=!MW_FORWARD_ARGS! "%~1""
shift /1
goto r_host_args

:r_host_launch
echo Running !HOST_EXE!
"!HOST_EXE!" !MW_FORWARD_ARGS!
set "RUN_RC=!ERRORLEVEL!"

popd
endlocal & exit /b %RUN_RC%


rem Resolve both single-config generators such as Ninja and multi-config
rem generators such as Visual Studio.
:resolve_host_exe
set "HOST_EXE="

if exist "%MW_ROOT%\%BUILD_DIR%\microwave_raylib.exe" (
    set "HOST_EXE=%MW_ROOT%\%BUILD_DIR%\microwave_raylib.exe"
    exit /b 0
)

if exist "%MW_ROOT%\%BUILD_DIR%\Release\microwave_raylib.exe" (
    set "HOST_EXE=%MW_ROOT%\%BUILD_DIR%\Release\microwave_raylib.exe"
    exit /b 0
)

exit /b 0


rem ---------------------------------------------------------------------------
rem DOS frontend
rem ---------------------------------------------------------------------------

:r_dos
if not exist "build-dos\MWDEMO.EXE" (
    call "%MW_ROOT%\scripts\mw_build.bat" dos
    if errorlevel 1 goto fail
)

call "%MW_ROOT%\scripts\mw_tools.bat" dosbox
if errorlevel 1 goto fail

if "%MW_DOSBOX_CYCLES%"=="" set "MW_DOSBOX_CYCLES=max"

rem Cycles matter more here than they do for the renderer. A mixer that keeps
rem up at "max" has told you about your host CPU and nothing about a 386.
"%MW_DOSBOX%" ^
    -c "config -set cpu cycles=%MW_DOSBOX_CYCLES%" ^
    -c "mount c build-dos" ^
    -c "c:" ^
    -c "set BLASTER=A220 I7 D1" ^
    -c "MWDEMO.EXE %1" ^
    -c "exit"

if errorlevel 1 goto fail
goto ok


rem ---------------------------------------------------------------------------

:fail
set "RUN_RC=%ERRORLEVEL%"
if "%RUN_RC%"=="0" set "RUN_RC=1"
popd
endlocal & exit /b %RUN_RC%

:ok
popd
endlocal
exit /b 0