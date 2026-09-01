@echo off
rem Locate the external toolchains once, so no other script hardcodes a path.
rem Call with the tool name; sets the matching variable in the caller's scope.
rem   call mw_tools.bat dosbox   -> MW_DOSBOX
rem   call mw_tools.bat watcom   -> MW_WATCOM
rem   call mw_tools.bat cmake    -> MW_CMAKE
rem   call mw_tools.bat python   -> MW_PYTHON
rem Returns 1 if the tool could not be found.
rem
rem Deliberately the same shape as MicroRender's mr_tools.bat. Raylib is
rem absent on purpose: it is resolved by microwave_raylib/CMakeLists.txt, which
rem prefers the pinned submodule and falls back to an installed package, so
rem there is no path for a batch file to find.

if /i "%~1"=="dosbox" goto find_dosbox
if /i "%~1"=="watcom" goto find_watcom
if /i "%~1"=="cmake"  goto find_cmake
if /i "%~1"=="python" goto find_python
echo mw_tools.bat: unknown tool "%~1"
exit /b 1

rem ---------------------------------------------------------------------------
:find_dosbox
if not "%DOSBOX_EXE%"=="" (
    rem The parentheses matter. Without them, "&" separates commands at parse
    rem time and the exit runs whether or not the file exists, which would
    rem return success with MW_DOSBOX unset and make the warning unreachable.
    if exist "%DOSBOX_EXE%" (
        set "MW_DOSBOX=%DOSBOX_EXE%"
        exit /b 0
    )
    echo WARNING: DOSBOX_EXE is set but "%DOSBOX_EXE%" does not exist.
    echo          Falling back to PATH and the usual install folders.
)
set "MW_DOSBOX="
for %%E in (dosbox-x.exe dosbox.exe DOSBox.exe) do (
    if not defined MW_DOSBOX (
        for /f "delims=" %%P in ('where %%E 2^>nul') do (
            if not defined MW_DOSBOX set "MW_DOSBOX=%%P"
        )
    )
)
for %%D in (
    "%ProgramFiles%\DOSBox-X\dosbox-x.exe"
    "%ProgramFiles(x86)%\DOSBox-X\dosbox-x.exe"
    "%ProgramFiles%\DOSBox-0.74-3\DOSBox.exe"
    "%ProgramFiles(x86)%\DOSBox-0.74-3\DOSBox.exe"
    "%ProgramFiles%\DOSBox-0.74\DOSBox.exe"
    "%ProgramFiles(x86)%\DOSBox-0.74\DOSBox.exe"
) do (
    if not defined MW_DOSBOX if exist %%D set "MW_DOSBOX=%%~D"
)
if not defined MW_DOSBOX (
    echo ERROR: DOSBox not found on PATH or in the usual install folders.
    echo Set DOSBOX_EXE to the full path of dosbox-x.exe or dosbox.exe.
    exit /b 1
)
exit /b 0

rem ---------------------------------------------------------------------------
rem MicroWave's DOS frontend is built with wcl, but wcc is the compiler proper
rem and is what confirms a usable install. binnt64 is checked first because a
rem modern Open Watcom 2.0 install has it and the 32-bit binnt is the fallback.
:find_watcom
if "%WATCOM%"=="" (
    echo ERROR: WATCOM is not set. Point it at your Open Watcom install root.
    echo        set WATCOM=C:\WATCOM
    exit /b 1
)
if exist "%WATCOM%\binnt64\wcc.exe" (
    set "MW_WATCOM=%WATCOM%"
    exit /b 0
)
if exist "%WATCOM%\binnt\wcc.exe" (
    set "MW_WATCOM=%WATCOM%"
    exit /b 0
)
if exist "%WATCOM%\binw\wcc.exe" (
    set "MW_WATCOM=%WATCOM%"
    exit /b 0
)
echo ERROR: wcc.exe not found under "%WATCOM%" (looked in binnt64\, binnt\ and binw\).
exit /b 1

rem ---------------------------------------------------------------------------
:find_cmake
set "MW_CMAKE="
for /f "delims=" %%P in ('where cmake 2^>nul') do (
    if not defined MW_CMAKE set "MW_CMAKE=%%P"
)
if not defined MW_CMAKE (
    echo ERROR: cmake not found on PATH.
    exit /b 1
)
exit /b 0

rem ---------------------------------------------------------------------------
rem Only the asset packer needs Python, so this is not fatal for a source
rem build. The demo song is synthesized and needs no pack at all.
:find_python
set "MW_PYTHON="
for %%E in (python.exe python3.exe py.exe) do (
    if not defined MW_PYTHON (
        for /f "delims=" %%P in ('where %%E 2^>nul') do (
            if not defined MW_PYTHON set "MW_PYTHON=%%P"
        )
    )
)
if not defined MW_PYTHON (
    echo ERROR: python not found on PATH. It is only needed for the asset
    echo        packer; the demo song is synthesized and needs no assets.
    exit /b 1
)
exit /b 0
