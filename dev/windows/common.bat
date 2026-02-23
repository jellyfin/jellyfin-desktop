@echo off
REM Jellyfin Desktop - Common variables
REM Sourced by other scripts

set QT_VERSION=6.10.1
set MPV_RELEASE=20260223
set MPV_VERSION=20260223-git-f439c7f
set SCRIPT_DIR=%~dp0
for %%i in ("%SCRIPT_DIR%\..\..") do set "PROJECT_ROOT=%%~fi"
set DEPS_DIR=%SCRIPT_DIR%deps
set BUILD_DIR=%PROJECT_ROOT%\build
set EXE_NAME=Jellyfin Desktop.exe

REM === Find Visual Studio ===
set VCVARS=
set "VS_BT=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "VS_CM=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "VS_PR=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
set "VS_EN=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
set "VS_BT86=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if exist "%VS_BT%" set "VCVARS=%VS_BT%"
if not defined VCVARS if exist "%VS_CM%" set "VCVARS=%VS_CM%"
if not defined VCVARS if exist "%VS_PR%" set "VCVARS=%VS_PR%"
if not defined VCVARS if exist "%VS_EN%" set "VCVARS=%VS_EN%"
if not defined VCVARS if exist "%VS_BT86%" set "VCVARS=%VS_BT86%"
