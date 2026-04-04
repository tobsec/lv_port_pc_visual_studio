@echo off
setlocal

set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"

if not exist %MSBUILD% (
    echo MSBuild not found at %MSBUILD%
    echo Searching...
    for /f "delims=" %%i in ('where msbuild 2^>nul') do set MSBUILD="%%i"
)

echo Building with %MSBUILD%
%MSBUILD% "%~dp0LVGL.slnx" /p:Configuration=Debug /p:Platform=x64 /m /v:minimal

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build succeeded. Run with:
    echo   sim\Output\Binaries\Debug\x64\LvglWindowsSimulator.exe
) else (
    echo.
    echo Build FAILED.
)
