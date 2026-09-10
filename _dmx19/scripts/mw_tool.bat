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
    echo   .\mw.bat tool opl-wav --wad FILE --music LUMP --out FILE [options]
    echo.
    echo genmidi-wav:
    echo   Existing lightweight generic FM validator and headroom scanner.
    echo.
    echo opl-wav:
    echo   Register-native Doom GENMIDI renderer using Nuked OPL3 in
    echo   nine-channel OPL2 compatibility mode.
    echo.
    echo opl-wav options:
    echo   --seconds N      render length, default 60
    echo   --rate N         output rate, default 22050
    echo   --block N        mixer block size, default 256
    echo   --gain N         OPL output gain, 0..256, default 256
    echo   --freq-split N   284 default, or 283 DMX side-bug A/B
    echo   --trace FILE     write ordered OPL register calls as CSV
    echo   --accum MODE     wide or saturating
    echo   --no-loop        stop MUS at its first end marker
    exit /b 0
)

if /i "!WHAT!"=="build" (
    echo === MicroWave host tools ===

    if not exist "%MW_ROOT%\third_party\nuked-opl3\opl3.c" (
        echo === Vendoring pinned Nuked OPL3 ===
        powershell -NoProfile -ExecutionPolicy Bypass -File "%MW_ROOT%\scripts\mw_vendor_nuked_opl3.ps1"
        if errorlevel 1 exit /b 1
    )
    if not exist "%MW_ROOT%\third_party\nuked-opl3\opl3.h" (
        echo ERROR: Nuked OPL3 header was not produced.
        exit /b 1
    )

    call "%MW_ROOT%\scripts\mw_tools.bat" cmake
    if errorlevel 1 exit /b 1

    "!MW_CMAKE!" -S "%MW_ROOT%\tools" -B "%MW_ROOT%\build-tools"
    if errorlevel 1 exit /b 1
    "!MW_CMAKE!" --build "%MW_ROOT%\build-tools" --config Release
    if errorlevel 1 exit /b 1

    set "MW_GENERIC_EXE="
    if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe" set "MW_GENERIC_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe"
    if not defined MW_GENERIC_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav.exe" set "MW_GENERIC_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav.exe"
    if not defined MW_GENERIC_EXE if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav" set "MW_GENERIC_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav"
    if not defined MW_GENERIC_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav" set "MW_GENERIC_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav"

    set "MW_OPL_EXE="
    if exist "%MW_ROOT%\build-tools\Release\mw_opl_wav.exe" set "MW_OPL_EXE=%MW_ROOT%\build-tools\Release\mw_opl_wav.exe"
    if not defined MW_OPL_EXE if exist "%MW_ROOT%\build-tools\mw_opl_wav.exe" set "MW_OPL_EXE=%MW_ROOT%\build-tools\mw_opl_wav.exe"
    if not defined MW_OPL_EXE if exist "%MW_ROOT%\build-tools\Release\mw_opl_wav" set "MW_OPL_EXE=%MW_ROOT%\build-tools\Release\mw_opl_wav"
    if not defined MW_OPL_EXE if exist "%MW_ROOT%\build-tools\mw_opl_wav" set "MW_OPL_EXE=%MW_ROOT%\build-tools\mw_opl_wav"

    if not defined MW_GENERIC_EXE (
        echo ERROR: mw_genmidi_wav executable was not produced.
        exit /b 1
    )
    if not defined MW_OPL_EXE (
        echo ERROR: mw_opl_wav executable was not produced.
        exit /b 1
    )

    echo   built !MW_GENERIC_EXE!
    echo   built !MW_OPL_EXE!
    exit /b 0
)

if /i "!WHAT!"=="genmidi-wav" (
    set "TOOL_ARGS=!MW_TOOL_ORIGINAL_ARGS:*genmidi-wav=!"

    set "MW_TOOL_EXE="
    if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\Release\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_genmidi_wav"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_genmidi_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_genmidi_wav"

    if not defined MW_TOOL_EXE (
        call "%~f0" build
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

if /i "!WHAT!"=="opl-wav" (
    set "TOOL_ARGS=!MW_TOOL_ORIGINAL_ARGS:*opl-wav=!"

    set "MW_TOOL_EXE="
    if exist "%MW_ROOT%\build-tools\Release\mw_opl_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_opl_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_opl_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_opl_wav.exe"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\Release\mw_opl_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_opl_wav"
    if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_opl_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_opl_wav"

    if not defined MW_TOOL_EXE (
        call "%~f0" build
        if errorlevel 1 exit /b 1

        if exist "%MW_ROOT%\build-tools\Release\mw_opl_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_opl_wav.exe"
        if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_opl_wav.exe" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_opl_wav.exe"
        if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\Release\mw_opl_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\Release\mw_opl_wav"
        if not defined MW_TOOL_EXE if exist "%MW_ROOT%\build-tools\mw_opl_wav" set "MW_TOOL_EXE=%MW_ROOT%\build-tools\mw_opl_wav"
    )

    if not defined MW_TOOL_EXE (
        echo ERROR: mw_opl_wav executable was not produced.
        exit /b 1
    )

    "!MW_TOOL_EXE!" !TOOL_ARGS!
    exit /b !ERRORLEVEL!
)

echo ERROR: unknown tool "!WHAT!".
echo Use ".\mw.bat tool" for help.
exit /b 1
