@echo off
REM Jellyfin Desktop - Windows build script
REM Run setup.bat first to install dependencies

setlocal enabledelayedexpansion
call "%~dp0common.bat" || exit /b 1

REM === Check dependencies ===
if not exist "%DEPS_DIR%\mpv\libmpv-2.dll" (
    echo ERROR: libmpv not found. Run setup.bat first
    exit /b 1
)
if not exist "%DEPS_DIR%\vcruntime" (
    echo ERROR: VC runtime DLLs not found. Run setup.bat first
    exit /b 1
)

REM === Find Qt ===
set "QTROOT=%QTROOT_WIN:\=/%"
if not exist "%QTROOT_WIN%" (
    echo ERROR: Qt not found at %QTROOT_WIN%
    echo Run setup.bat first
    exit /b 1
)
echo Using Qt: %QTROOT%

REM === Check Visual Studio ===
if not defined VCVARS (
    echo ERROR: %VCVARS_NAME% not found. Install the MSVC %JELLYFIN_ARCH% build tools.
    exit /b 1
)
echo Using: %VCVARS%

REM === Initialize VS environment (needed for lib/dumpbin) ===
call "%VCVARS%"
if errorlevel 1 (
    echo ERROR: Failed to initialize VS environment
    exit /b 1
)

REM === Setup build directory ===
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

REM === Generate import library if needed ===
if not exist "%DEPS_DIR%\mpv\libmpv-2.dll.lib" (
    echo Generating import library...
    powershell -NoProfile -Command "$exports = & dumpbin /nologo /exports ($env:DEPS_DIR + '\mpv\libmpv-2.dll'); if ($LASTEXITCODE) { exit $LASTEXITCODE }; $names = @($exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)') { $Matches[1] } }); if (-not $names.Count) { Write-Error 'No libmpv exports found'; exit 1 }; @('LIBRARY libmpv-2.dll', 'EXPORTS') + $names | Set-Content -Encoding ascii -LiteralPath ($env:DEPS_DIR + '\mpv\mpv.def')"
    if errorlevel 1 exit /b 1
    lib /def:"%DEPS_DIR%\mpv\mpv.def" /out:"%DEPS_DIR%\mpv\libmpv-2.dll.lib" /MACHINE:%MACHINE%
    if errorlevel 1 (
        echo ERROR: Failed to create import library
        exit /b 1
    )
)

REM === Configure ===
set "DEPS_CMAKE=%DEPS_DIR:\=/%"
echo Configuring...
cmake -GNinja ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_INSTALL_PREFIX=output ^
    -DQTROOT="%QTROOT%" ^
    -DMPV_INCLUDE_DIR="%DEPS_CMAKE%/mpv/include" ^
    -DMPV_LIBRARY="%DEPS_CMAKE%/mpv/libmpv-2.dll.lib" ^
    -DWINDOWS_DEPENDENCY_DIR="%DEPS_CMAKE%" ^
    -DCHECK_FOR_UPDATES=ON ^
    -DUSE_STATIC_MPVQT=ON ^
    "%PROJECT_ROOT%"
if errorlevel 1 (
    echo ERROR: CMake configuration failed
    exit /b 1
)

REM === Build ===
echo Building...
ninja
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo Build complete!
echo Executable: %BUILD_DIR%\src\%EXE_NAME%
endlocal
