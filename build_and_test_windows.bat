@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem The gate: run this (double-click is fine) before considering any change
rem done. Mirrors build_and_test_linuxmac.sh - see that script's header for why
rem core-only runs first. This uses the Ninja generator on Windows too, not
rem the Visual Studio solution generator, so cl.exe/link.exe need to be on
rem PATH; if they aren't, this tries to find and call vcvarsall.bat itself
rem so a plain double-click works without a Developer Command Prompt.

where cl >nul 2>&1
if errorlevel 1 (
    echo cl.exe not on PATH - looking for a Visual Studio install...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo Could not find vswhere.exe. Install Visual Studio 2022 ^(or the standalone
        echo Build Tools^) with the "Desktop development with C++" workload, or run this
        echo from a "Developer Command Prompt for VS 2022" instead.
        goto :fail
    )
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
    if not defined VSINSTALL (
        echo Found Visual Studio but not the C++ toolset. Install the "Desktop
        echo development with C++" workload.
        goto :fail
    )
    call "!VSINSTALL!\VC\Auxiliary\Build\vcvarsall.bat" x64
    if errorlevel 1 goto :fail
)

where ninja >nul 2>&1
if errorlevel 1 (
    echo ninja.exe not found on PATH. Install Ninja and add it to PATH.
    goto :fail
)

where python >nul 2>&1
if errorlevel 1 (
    echo python not found on PATH. Install Python 3 and add it to PATH.
    goto :fail
)

echo == module boundary check ==
python tools\check_module_boundaries.py
if errorlevel 1 goto :fail

echo == core-only: configure ==
cmake --preset core-only
if errorlevel 1 goto :fail
echo == core-only: build ==
ninja -C build-core-only
if errorlevel 1 goto :fail
echo == core-only: test ==
ctest --preset core-only -L fast
if errorlevel 1 goto :fail

echo == windows-msvc: configure ==
cmake --preset windows-msvc
if errorlevel 1 goto :fail
echo == windows-msvc: build ==
ninja -C build-windows-msvc
if errorlevel 1 goto :fail
echo == windows-msvc: test ==
ctest --preset windows-msvc -L fast
if errorlevel 1 goto :fail

echo == windows-msvc: test app (JUCE-linked, e.g. jamn_app_smoke) ==
ctest --preset windows-msvc -L app
if errorlevel 1 goto :fail

echo.
echo All checks passed.
pause
exit /b 0

:fail
echo.
echo FAILED - see output above.
pause
exit /b 1
