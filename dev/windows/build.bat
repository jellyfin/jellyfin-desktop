@echo off
REM Find vcvars64.bat from vswhere (supports Community, Professional, Enterprise, BuildTools)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo Visual Studio installation not found.
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
