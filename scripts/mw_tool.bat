@echo off
rem Host-only MicroWave utility runner. Invoked through mw.bat.
rem
rem This file intentionally uses no GOTO labels or CALL :subroutines. The host
rem tool dispatcher is small enough that straight-line branches are clearer,
rem and this avoids cmd.exe label-scanning problems in files introduced through
rem patch workflows with mixed line endings.
setlocal EnableExtensions EnableDelayedExpansion
if "%MW_ROOT%"=="" set "MW_ROOT=%~dp0.."
cd /d "%MW_ROOT%"

set "MW_TOOL_ORIGINAL_ARGS=%*"
set "WHAT="
if /i "%~1"=="tool" (
    set "WHAT=%~2"
) else (
    set "WHAT=%~1"
)

if not defined WHAT (
    echo MicroWave host tools.
    echo.
    echo   .\mw.bat tool build
    echo   .\mw.bat tool genmidi-wav --wad FILE --music LUMP --out FILE [options]
    echo.
    echo genmidi-wav options:
    echo   --seconds N      render length, default 30
    echo   --rate N         sample rate, default 22050
    echo   --block N        mixer block size, default 256
    echo   --voices N       FM voice limit, default 9, maximum 18
    echo   --no-loop        stop MUS at its first end marker instead of looping
    exit /b 0
)

if /i "!WHAT!"=="build" (
    echo === MicroWave host tools ===
    call "%MW_ROOT%\scripts\mw_tools.bat" cmake
    if errorlevel 1 exit /b 1

    rem Visual Studio is a multi-config generator, so Release is selected at
    rem build time with --config rather than CMAKE_BUILD_TYPE at configure time.
    "!MW_CMAKE!" -S "%MW_ROOT%\tools" -B "%MW_ROOT%\build-tools"
    if errorlevel 1 exit /b 1
    "!MW_CMAKE!" --build "%MW_ROOT%\build-tools" --config Release
    if errorlevel 1 exit /b 1

    set "MW_TOOL_EXE="
    if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav"

    if not defined MW_TOOL_EXE (
        echo ERROR: mw_genmidi_wav executable was not produced.
        exit /b 1
    )
    echo   built !MW_TOOL_EXE!
    exit /b 0
)

if /i "!WHAT!"=="genmidi-wav" (
    rem Preserve the user's quoted argument tail exactly enough for normal
    rem Windows paths: remove everything through the dispatcher token and pass
    rem the rest straight to the executable.
    set "TOOL_ARGS=!MW_TOOL_ORIGINAL_ARGS:*genmidi-wav=!"

    set "MW_TOOL_EXE="
    if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav"

    if not defined MW_TOOL_EXE (
        echo === MicroWave host tools ===
        call "%MW_ROOT%\scripts\mw_tools.bat" cmake
        if errorlevel 1 exit /b 1
        "!MW_CMAKE!" -S "%MW_ROOT%\tools" -B "%MW_ROOT%\build-tools"
        if errorlevel 1 exit /b 1
        "!MW_CMAKE!" --build "%MW_ROOT%\build-tools" --config Release
        if errorlevel 1 exit /b 1

        if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe"
        if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav.exe"
        if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav"
        if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav"
    )

    if not defined MW_TOOL_EXE (
        echo ERROR: mw_genmidi_wav executable was not produced.
        exit /b 1
    )

    "!MW_TOOL_EXE!" !TOOL_ARGS!
    exit /b !ERRORLEVEL!
)

echo ERROR: unknown tool "!WHAT!".
echo Use ".\mw.bat tool" for help.
exit /b 1
