@echo off
rem Remove every build output. Sources and assets are never touched.
setlocal EnableExtensions
set "MW_ROOT=%~dp0.."
pushd "%MW_ROOT%"

for %%D in (build-tests build-tests-u8 build-tests-narrow build-bench build-headless build-raylib build-dos build-pico) do (
    if exist "%%D" (
        echo   removing %%D
        rmdir /s /q "%%D"
    )
)

if exist "GAME.MWP" ( echo   removing GAME.MWP & del /q "GAME.MWP" )

echo clean.
popd
endlocal
exit /b 0
