@echo off
REM Find vcvars64.bat from vswhere (supports Community, Professional, Enterprise, BuildTools)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo Visual Studio installation not found.
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

REM bindgen (used by jfn-mpv's build.rs) dlopens libclang at build time.
REM mingw-w64-clang-x86_64-llvm ships libclang.dll under clang64\bin; point
REM bindgen at it. Mirror the GitHub Actions Windows job (build-windows.yml).
set "MSYS_BIN=C:\msys64\clang64\bin"
set "LIBCLANG_PATH=%MSYS_BIN%"
set "BINDGEN_EXTRA_CLANG_ARGS=--target=x86_64-pc-windows-msvc"
REM Append (not prepend) so MSVC compilers from vcvars stay primary.
set "PATH=%PATH%;%MSYS_BIN%"

"C:\Program Files\PowerShell\7\pwsh.exe" -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
