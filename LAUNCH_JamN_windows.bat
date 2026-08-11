@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem Runs the already-built jamn_app.exe from the windows-msvc preset
rem (build-windows-msvc\). If the binary is missing or stale, build it first:
rem build_and_test_windows.bat. Any arguments are passed through,
rem e.g. `LAUNCH_JamN_windows.bat --headless`.

set "BIN=build-windows-msvc\modules\jamn_app\jamn_app.exe"

if not exist "%BIN%" (
    echo %BIN% not found.
    echo Build it first: build_and_test_windows.bat
    pause
    exit /b 1
)

"%BIN%" %*
