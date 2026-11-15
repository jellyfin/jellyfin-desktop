@echo off
REM Jellyfin Desktop - Windows bundling script
REM Run build.bat first

setlocal enabledelayedexpansion
call "%~dp0common.bat" || exit /b 1

REM === Check build exists ===
if not exist "%BUILD_DIR%\src\%EXE_NAME%" (
    echo ERROR: Build not found. Run build.bat first
    exit /b 1
)

REM === Check Visual Studio ===
if not defined VCVARS (
    echo ERROR: %VCVARS_NAME% not found
    exit /b 1
)
echo Using: %VCVARS%

REM === Initialize VS environment ===
call "%VCVARS%"
if errorlevel 1 (
    echo ERROR: Failed to initialize VS environment
    exit /b 1
)

REM === Add tools to PATH ===
set "PATH=%PATH%;%LocalAppData%\Programs\Inno Setup 6"
set "PATH=%PATH%;C:\Program Files\Git\cmd"

cd /d "%BUILD_DIR%"

REM === Package ===
echo Creating installer and portable ZIP...
ninja windows_all
if errorlevel 1 (
    echo ERROR: Package creation failed
    exit /b 1
)

echo.
echo Bundle complete!
echo Installer: %BUILD_DIR%\JellyfinDesktop-*.exe
echo Portable:  %BUILD_DIR%\JellyfinDesktop-*.zip
endlocal
