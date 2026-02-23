@echo off
REM Jellyfin Desktop - Windows dependency installer
REM Run once to install all build dependencies

setlocal EnableDelayedExpansion
call "%~dp0common.bat"

set "7ZIP=C:\Program Files\7-Zip\7z.exe"
if not exist "%7ZIP%" (
    for /f "delims=" %%i in ('where 7z.exe 2^>nul') do (
        set "7ZIP=%%i"
        goto :found7zip
    )
)
:found7zip

if not exist "%DEPS_DIR%" mkdir "%DEPS_DIR%"

echo [1/14] Installing CMake...
winget install --id Kitware.CMake --accept-package-agreements --accept-source-agreements --silent
if errorlevel 1 echo Warning: CMake may already be installed

echo [2/14] Installing Ninja...
winget install --id Ninja-build.Ninja --accept-package-agreements --accept-source-agreements --silent
if errorlevel 1 echo Warning: Ninja may already be installed

echo [3/14] Installing 7-Zip...
winget install --id 7zip.7zip --accept-package-agreements --accept-source-agreements --silent
if errorlevel 1 echo Warning: 7-Zip may already be installed

echo [4/14] Installing aqtinstall...
winget install --id miurahr.aqtinstall --accept-package-agreements --accept-source-agreements --silent
if errorlevel 1 echo Warning: aqtinstall may already be installed

echo [5/14] Installing Visual Studio 2022 Build Tools...
winget install --id Microsoft.VisualStudio.2022.BuildTools ^
  --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" ^
  --accept-package-agreements --accept-source-agreements --silent
if errorlevel 1 echo Warning: VS Build Tools may already be installed

echo [6/14] Installing MinGW (for gendef)...
winget install --id mingw.mingw-w64-ucrt --accept-package-agreements --accept-source-agreements --silent
if errorlevel 1 (
    echo Warning: mingw.mingw-w64-ucrt not available, trying msys2.msys2...
    winget install --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements --silent
    if errorlevel 1 echo Warning: MinGW/MSYS2 install skipped. gendef fallback may run during build.
)

echo [7/14] Installing Inno Setup...
winget install --id JRSoftware.InnoSetup --accept-package-agreements --accept-source-agreements --silent
if errorlevel 1 echo Warning: Inno Setup may already be installed

echo [8/14] Installing Qt %QT_VERSION%...
if not exist "%DEPS_DIR%\qt\%QT_VERSION%\msvc2022_64" (
    pushd "%DEPS_DIR%"
    aqt install-qt windows desktop %QT_VERSION% win64_msvc2022_64 -m qtwebengine qtwebchannel qtpositioning -O "qt"
    popd
) else (
    echo Qt already installed, skipping
)

echo [9/14] Downloading libmpv AVX2...
if not exist "%DEPS_DIR%\mpv\libmpv-2.dll" (
    echo Downloading libmpv...
    if exist "%DEPS_DIR%\mpv" rmdir /s /q "%DEPS_DIR%\mpv"
    if exist "%DEPS_DIR%\mpv_tmp" rmdir /s /q "%DEPS_DIR%\mpv_tmp"
    curl -L "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/%MPV_RELEASE%/mpv-dev-x86_64-v3-%MPV_VERSION%.7z" -o "%DEPS_DIR%\mpv.7z"
    if errorlevel 1 (
        echo ERROR: Failed to download libmpv
    ) else (
        if not exist "%7ZIP%" (
            echo ERROR: 7-Zip not found. Please install 7-Zip and re-run setup.
        ) else (
        "%7ZIP%" x "%DEPS_DIR%\mpv.7z" -o"%DEPS_DIR%\mpv_tmp" -y
        if errorlevel 1 (
            echo ERROR: Failed to extract libmpv archive
        ) else (
        set "MPV_SRC=%DEPS_DIR%\mpv_tmp"
        if not exist "%MPV_SRC%\libmpv-2.dll" (
            for /d %%D in ("%DEPS_DIR%\mpv_tmp\*") do (
                if exist "%%~fD\libmpv-2.dll" set "MPV_SRC=%%~fD"
            )
        )
        if exist "%MPV_SRC%\libmpv-2.dll" (
        mkdir "%DEPS_DIR%\mpv"
        if exist "%MPV_SRC%\include" xcopy /e /i /y "%MPV_SRC%\include" "%DEPS_DIR%\mpv\include\" >nul
        move /y "%MPV_SRC%\libmpv-2.dll" "%DEPS_DIR%\mpv\" >nul
        if exist "%MPV_SRC%\libmpv.dll.a" move /y "%MPV_SRC%\libmpv.dll.a" "%DEPS_DIR%\mpv\" >nul
        if exist "%MPV_SRC%\libmpv-2.dll.lib" move /y "%MPV_SRC%\libmpv-2.dll.lib" "%DEPS_DIR%\mpv\" >nul
        ) else (
            echo ERROR: libmpv-2.dll not found in extracted archive
        )
        rmdir /s /q "%DEPS_DIR%\mpv_tmp"
        del "%DEPS_DIR%\mpv.7z"
        echo libmpv extracted to %DEPS_DIR%\mpv
        )
        )
    )
) else (
    echo libmpv already installed, skipping
)

echo [10/14] Downloading libmpv fallback (non-AVX2)...
if not exist "%DEPS_DIR%\mpv-fallback\libmpv-2.dll" (
    if exist "%DEPS_DIR%\mpv-fallback-tmp" rmdir /s /q "%DEPS_DIR%\mpv-fallback-tmp"
    curl -L "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/%MPV_RELEASE%/mpv-dev-x86_64-%MPV_VERSION%.7z" -o "%DEPS_DIR%\mpv-fallback.7z"
    if errorlevel 1 (
        echo ERROR: Failed to download libmpv fallback
    ) else (
        if not exist "%7ZIP%" (
            echo ERROR: 7-Zip not found. Please install 7-Zip and re-run setup.
        ) else (
        "%7ZIP%" x "%DEPS_DIR%\mpv-fallback.7z" -o"%DEPS_DIR%\mpv-fallback-tmp" -y
        if errorlevel 1 (
            echo ERROR: Failed to extract libmpv fallback archive
        ) else (
        set "MPV_FALLBACK_SRC=%DEPS_DIR%\mpv-fallback-tmp"
        if not exist "%MPV_FALLBACK_SRC%\libmpv-2.dll" (
            for /d %%D in ("%DEPS_DIR%\mpv-fallback-tmp\*") do (
                if exist "%%~fD\libmpv-2.dll" set "MPV_FALLBACK_SRC=%%~fD"
            )
        )
        mkdir "%DEPS_DIR%\mpv-fallback"
        if exist "%MPV_FALLBACK_SRC%\libmpv-2.dll" (
            move /y "%MPV_FALLBACK_SRC%\libmpv-2.dll" "%DEPS_DIR%\mpv-fallback\" >nul
            if exist "%MPV_FALLBACK_SRC%\libmpv.dll.a" move /y "%MPV_FALLBACK_SRC%\libmpv.dll.a" "%DEPS_DIR%\mpv-fallback\" >nul
        ) else (
            echo ERROR: libmpv-2.dll not found in fallback archive
        )
        rmdir /s /q "%DEPS_DIR%\mpv-fallback-tmp"
        del "%DEPS_DIR%\mpv-fallback.7z"
        echo libmpv fallback extracted to %DEPS_DIR%\mpv-fallback
        )
        )
    )
) else (
    echo libmpv fallback already installed, skipping
)

echo [11/14] Downloading VCRedist and WiX tools...
if not exist "%DEPS_DIR%\vc_redist.x64.exe" (
    echo Downloading vc_redist.x64.exe...
    curl -L -o "%DEPS_DIR%\vc_redist.x64.exe" https://aka.ms/vs/17/release/vc_redist.x64.exe
)
if not exist "%DEPS_DIR%\wix" (
    echo Downloading WiX tools...
    curl -L -o "%DEPS_DIR%\wix.zip" https://github.com/wixtoolset/wix3/releases/download/wix3111rtm/wix311-binaries.zip
    mkdir "%DEPS_DIR%\wix"
    %7ZIP% x -y "%DEPS_DIR%\wix.zip" -o"%DEPS_DIR%\wix"
    del "%DEPS_DIR%\wix.zip"
)

echo [12/14] Extracting VC runtime DLLs...
if not exist "%DEPS_DIR%\vcruntime" (
    echo Extracting VCRedist with dark.exe...
    mkdir "%DEPS_DIR%\vcruntime"
    "%DEPS_DIR%\wix\dark.exe" -nologo "%DEPS_DIR%\vc_redist.x64.exe" -x "%DEPS_DIR%\vcredist_tmp"
    echo Extracting runtime CABs...
    expand.exe -F:* "%DEPS_DIR%\vcredist_tmp\AttachedContainer\packages\vcRuntimeMinimum_amd64\cab1.cab" "%DEPS_DIR%\vcruntime"
    expand.exe -F:* "%DEPS_DIR%\vcredist_tmp\AttachedContainer\packages\vcRuntimeAdditional_amd64\cab1.cab" "%DEPS_DIR%\vcruntime"
    REM Rename files from *_amd64 to *.dll
    for %%f in ("%DEPS_DIR%\vcruntime\*_amd64") do (
        set "name=%%~nf"
        ren "%%f" "!name:_amd64=!.dll"
    )
    rd /s /q "%DEPS_DIR%\vcredist_tmp"
    echo VC runtime DLLs extracted to %DEPS_DIR%\vcruntime
)

echo [13/14] Generating mpv import library...
if exist "%DEPS_DIR%\mpv\libmpv-2.dll" (
    if not exist "%DEPS_DIR%\mpv\libmpv-2.dll.lib" (
        REM Find Visual Studio
        set "VCVARS="
        for %%v in (
            "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
            "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
            "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        ) do (
            if exist %%v set "VCVARS=%%~v"
        )
        if defined VCVARS (
            echo Using Visual Studio: !VCVARS!
            call "!VCVARS!" >nul 2>&1
            echo Generating import library...
            echo LIBRARY libmpv-2.dll > "%DEPS_DIR%\mpv\mpv.def"
            echo EXPORTS >> "%DEPS_DIR%\mpv\mpv.def"
            for /f "skip=19 tokens=4" %%a in ('dumpbin /exports "%DEPS_DIR%\mpv\libmpv-2.dll"') do (
                if not "%%a"=="" echo %%a >> "%DEPS_DIR%\mpv\mpv.def"
            )
            lib /def:"%DEPS_DIR%\mpv\mpv.def" /out:"%DEPS_DIR%\mpv\libmpv-2.dll.lib" /MACHINE:X64
            echo Import library created: %DEPS_DIR%\mpv\libmpv-2.dll.lib
        ) else (
            echo WARNING: Visual Studio not found. Import library will be generated during build.
        )
    ) else (
        echo mpv import library already exists, skipping
    )
)

echo [14/14] Generating CMakePresets.json...
powershell -Command "(Get-Content '%SCRIPT_DIR%..\CMakePresets.json.in' -Raw) -replace '@QT_VERSION@','%QT_VERSION%' -replace '@BREW_PREFIX@','' | Set-Content '%PROJECT_ROOT%\CMakePresets.json' -NoNewline"

echo.
echo Setup complete. Restart terminal to refresh PATH, then run build.bat
endlocal
