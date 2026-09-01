@echo off
rem Run dispatcher, called by mw.bat.
setlocal EnableExtensions
set "MW_ROOT=%~dp0.."
pushd "%MW_ROOT%"

set "WHAT=%~1"
shift /1

if /i "%WHAT%"=="headless" goto r_headless
if /i "%WHAT%"=="raylib"   goto r_raylib
if /i "%WHAT%"=="dos"      goto r_dos

echo ERROR: unknown run target "%WHAT%".
goto fail

:r_headless
if not exist "build-headless\microwave_raylib.exe" call "%MW_ROOT%\scripts\mw_build.bat" headless || goto fail
"build-headless\microwave_raylib.exe" %1 %2 %3 %4 %5 %6
goto ok

:r_raylib
if not exist "build-raylib\microwave_raylib.exe" call "%MW_ROOT%\scripts\mw_build.bat" raylib || goto fail
"build-raylib\microwave_raylib.exe" %1 %2 %3 %4 %5 %6
goto ok

:r_dos
if not exist "build-dos\MWDEMO.EXE" call "%MW_ROOT%\scripts\mw_build.bat" dos || goto fail
call "%MW_ROOT%\scripts\mw_tools.bat" dosbox
if errorlevel 1 goto fail
if "%MW_DOSBOX_CYCLES%"=="" set "MW_DOSBOX_CYCLES=max"

rem Cycles matter more here than they do for the renderer. A mixer that keeps
rem up at "max" has told you about your host CPU and nothing about a 386.
"%MW_DOSBOX%" -c "config -set cpu cycles=%MW_DOSBOX_CYCLES%" -c "mount c build-dos" -c "c:" -c "set BLASTER=A220 I7 D1" -c "MWDEMO.EXE %1" -c "exit"
goto ok

:fail
popd
endlocal
exit /b 1

:ok
popd
endlocal
exit /b 0
