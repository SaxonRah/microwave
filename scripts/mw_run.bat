@echo off
rem MicroWave run driver. Invoked through mw.bat; see there for usage.
setlocal EnableExtensions EnableDelayedExpansion
if "%MW_ROOT%"=="" set "MW_ROOT=%~dp0.."
cd /d "%MW_ROOT%"

if /i "%~1"=="run" shift /1
set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=raylib"
shift /1

if /i "%WHAT%"=="headless" goto r_headless
if /i "%WHAT%"=="raylib"   goto r_raylib
if /i "%WHAT%"=="dos"      goto r_dos
if /i "%WHAT%"=="pico"     goto r_pico

echo ERROR: unknown run target "%WHAT%".
exit /b 1

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
    if errorlevel 1 exit /b 1
    call :resolve_host_exe
)
if not defined HOST_EXE (
    echo ERROR: %BUILD_TARGET% executable was not produced.
    exit /b 1
)
set "MW_FORWARD_ARGS="
:r_host_args
if "%~1"=="" goto r_host_launch
set "MW_FORWARD_ARGS=!MW_FORWARD_ARGS! "%~1""
shift /1
goto r_host_args
:r_host_launch
echo Running !HOST_EXE!
"!HOST_EXE!" !MW_FORWARD_ARGS!
exit /b %ERRORLEVEL%

:resolve_host_exe
set "HOST_EXE="
if exist "%MW_ROOT%\%BUILD_DIR%\microwave_raylib.exe" set "HOST_EXE=%MW_ROOT%\%BUILD_DIR%\microwave_raylib.exe"
if not defined HOST_EXE if exist "%MW_ROOT%\%BUILD_DIR%\Release\microwave_raylib.exe" set "HOST_EXE=%MW_ROOT%\%BUILD_DIR%\Release\microwave_raylib.exe"
exit /b 0

:r_pico
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=max98357a"
if not "%~1"=="" shift /1
set "METHOD=%~1"
if "%METHOD%"=="" set "METHOD=swd"
if not "%~1"=="" shift /1
set "MW_PICO_EXTRA="
:r_pico_args
if "%~1"=="" goto r_pico_launch
set "MW_PICO_EXTRA=!MW_PICO_EXTRA! "%~1""
shift /1
goto r_pico_args
:r_pico_launch
call "%MW_ROOT%\scripts\mw_tools.bat" python
if errorlevel 1 exit /b 1
"%MW_PYTHON%" "%MW_ROOT%\scripts\mw_pico.py" "%PRESET%" "%METHOD%" !MW_PICO_EXTRA!
exit /b %ERRORLEVEL%

:r_dos
if not exist "build-dos\MWDEMO.EXE" (
    call "%MW_ROOT%\scripts\mw_build.bat" dos
    if errorlevel 1 exit /b 1
)
call "%MW_ROOT%\scripts\mw_tools.bat" dosbox
if errorlevel 1 exit /b 1
if "%MW_DOSBOX_CYCLES%"=="" set "MW_DOSBOX_CYCLES=max"
"%MW_DOSBOX%" -c "config -set cpu cycles=%MW_DOSBOX_CYCLES%" -c "mount c build-dos" -c "c:" -c "set BLASTER=A220 I7 D1" -c "MWDEMO.EXE %1" -c "exit"
exit /b %ERRORLEVEL%
