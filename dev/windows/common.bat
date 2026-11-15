@echo off
REM Jellyfin Desktop - Common variables
REM Sourced by other scripts

set QT_VERSION=6.9.3
REM Keep in sync with .github/workflows/build-windows.yml
set MPV_VERSION=20261018-git-01e2533f31
set SCRIPT_DIR=%~dp0
for %%i in ("%SCRIPT_DIR%\..\..") do set "PROJECT_ROOT=%%~fi"
set DEPS_DIR=%SCRIPT_DIR%deps
set BUILD_DIR=%PROJECT_ROOT%\build
set EXE_NAME=Jellyfin Desktop.exe

set "HOST_ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "HOST_ARCH=%PROCESSOR_ARCHITEW6432%"
if not defined JELLYFIN_ARCH (
    set "JELLYFIN_ARCH=x64"
    if /i "%HOST_ARCH%"=="ARM64" set "JELLYFIN_ARCH=arm64"
)

set "QT_HOST=windows"
set "QT_ARCH=win64_msvc2022_64"
set "QT_ARCH_DIR=msvc2022_64"
set "MPV_ARCH=x86_64-v3"
set "MPV_ARCH_FALLBACK=x86_64"
set "MACHINE=X64"
set "VCVARS_NAME=vcvars64.bat"
if /i "%HOST_ARCH%"=="ARM64" set "VCVARS_NAME=vcvarsarm64_amd64.bat"

if /i "%JELLYFIN_ARCH%"=="arm64" (
    set "QT_VERSION=6.11.3"
    set "MPV_ARM64_VERSION=0.42.0"
    set "MPV_ARM64_SHA256=839e0f93492f2d4e66e6f2db3058000158de5935a4606863d419f5934d183932"
    set "QT_HOST=windows_arm64"
    set "QT_ARCH=win64_msvc2022_arm64"
    set "QT_ARCH_DIR=msvc2022_arm64"
    set "MPV_ARCH_FALLBACK="
    set "MACHINE=ARM64"
    set "DEPS_DIR=%SCRIPT_DIR%deps\arm64"
    set "BUILD_DIR=%PROJECT_ROOT%\build\arm64"
    set "VCVARS_NAME=vcvarsamd64_arm64.bat"
    if /i "%HOST_ARCH%"=="ARM64" set "VCVARS_NAME=vcvarsarm64.bat"
) else if /i not "%JELLYFIN_ARCH%"=="x64" (
    echo ERROR: JELLYFIN_ARCH must be x64 or arm64
    exit /b 1
)
set "QTROOT_WIN=%DEPS_DIR%\qt\%QT_VERSION%\%QT_ARCH_DIR%"

REM === Find Visual Studio ===
set VCVARS=
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%\VC\Auxiliary\Build\%VCVARS_NAME%" (
    set "VCVARS=%VSINSTALLDIR%\VC\Auxiliary\Build\%VCVARS_NAME%"
)
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%v in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
        if exist "%%v\VC\Auxiliary\Build\%VCVARS_NAME%" set "VCVARS=%%v\VC\Auxiliary\Build\%VCVARS_NAME%"
    )
)
if not defined VCVARS (
    for %%p in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
        for /d %%v in ("%%~p\Microsoft Visual Studio\*") do (
            for /d %%e in ("%%v\*") do (
                if exist "%%e\VC\Auxiliary\Build\%VCVARS_NAME%" set "VCVARS=%%e\VC\Auxiliary\Build\%VCVARS_NAME%"
            )
        )
    )
)

if "%~1"=="" exit /b 0
goto %~1

REM === Setup runtime PATH for DLLs ===
REM Call with: call "%~dp0common.bat" :setup_runtime
:setup_runtime
if not exist "%BUILD_DIR%" (
    echo ERROR: Build not found. Run build.bat first
    exit /b 1
)
set "PATH=%DEPS_DIR%\mpv;%PATH%"
set "PATH=%QTROOT_WIN%\bin;%PATH%"
exit /b 0
