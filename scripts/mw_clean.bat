@echo off
rem Remove every generated build directory. Sources and assets are untouched.
setlocal EnableExtensions
set "MW_ROOT=%~dp0.."
pushd "%MW_ROOT%"

for %%D in (build-tests build-tests-u8 build-tests-narrow build-bench build-headless build-raylib build-dos build-pico) do (
    if exist "%%D" (
        echo   removing %%D
        rmdir /s /q "%%D"
    )
)

for %%D in (build build-max98357a build-pcm5102a build-ns4168) do (
    if exist "microwave\%%D" (
        echo   removing microwave\%%D
        rmdir /s /q "microwave\%%D"
    )
)

if exist "GAME.MWP" (echo   removing GAME.MWP& del /q "GAME.MWP")

echo clean.
popd
endlocal
exit /b 0
