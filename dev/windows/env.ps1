# Shared MSVC + bindgen environment setup, dot-sourced by build.ps1 and
# the justfile's Windows clippy recipe. Loads vcvars64 (cl/INCLUDE/LIB)
# and points bindgen at msys64's libclang.

$ErrorActionPreference = "Stop"

if (-not $env:VSINSTALLDIR) {
    Write-Host "MSVC environment not detected." -ForegroundColor Yellow
    Write-Host "Attempting to load vcvars64.bat..."

    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $VcVars = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $VcVars) {
            $TempBat = Join-Path $env:TEMP "jfn_vcvars_build.bat"
            Set-Content $TempBat -Value ('@call "' + $VcVars + '"') -Encoding ASCII
            Add-Content $TempBat -Value '@set' -Encoding ASCII
            cmd /c $TempBat | ForEach-Object {
                if ($_ -match "^([^=]+)=(.*)$") {
                    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
                }
            }
            Remove-Item $TempBat -ErrorAction SilentlyContinue
            Write-Host "Loaded Visual Studio environment" -ForegroundColor Green
        }
    }

    if (-not $env:VSINSTALLDIR) {
        Write-Host "Could not load MSVC environment." -ForegroundColor Red
        Write-Host "Run from 'x64 Native Tools Command Prompt for VS 2022'"
        exit 1
    }
}

# bindgen (used by jfn-mpv's build.rs) dlopens libclang at build time.
# The mingw-w64-clang-*-llvm package ships libclang.dll under msys64's
# msystem prefix; point bindgen at it (mirrors .github/workflows/build-windows.yml).
if (-not $env:LIBCLANG_PATH) {
    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
    if ($arch -eq "arm64") {
        $MsysBin = "C:\msys64\clangarm64\bin"
        $Triple = "aarch64-pc-windows-msvc"
    } else {
        $MsysBin = "C:\msys64\clang64\bin"
        $Triple = "x86_64-pc-windows-msvc"
    }
    if (Test-Path (Join-Path $MsysBin "libclang.dll")) {
        $env:LIBCLANG_PATH = $MsysBin
        if (-not $env:BINDGEN_EXTRA_CLANG_ARGS) {
            $env:BINDGEN_EXTRA_CLANG_ARGS = "--target=$Triple"
        }
        $env:PATH = "$env:PATH;$MsysBin"
    } else {
        Write-Host "libclang.dll not found at $MsysBin" -ForegroundColor Red
        Write-Host "Run 'just deps' or install mingw-w64-clang-*-llvm via MSYS2 pacman."
        exit 1
    }
}

# jfn-mpv's build script resolves mpv (and, on Windows, ffmpeg) headers from
# EXTERNAL_MPV_DIR; without it, avcodec discovery falls back to pkg-config and
# finds msys64's mingw ffmpeg, whose headers don't parse under the MSVC target.
if (-not $env:EXTERNAL_MPV_DIR) {
    $RepoRootEnv = (Get-Item $PSScriptRoot).Parent.Parent.FullName
    foreach ($d in @("third_party\mpv-install", "third_party\mpv")) {
        $p = Join-Path $RepoRootEnv $d
        if (Test-Path (Join-Path $p "lib\mpv.lib")) {
            $env:EXTERNAL_MPV_DIR = $p
            break
        }
    }
}

# Test executables link mpv/ffmpeg dynamically; their DLLs ship in
# EXTERNAL_MPV_DIR\lib, which isn't on PATH by default.
if ($env:EXTERNAL_MPV_DIR) {
    $env:PATH = "$env:PATH;$env:EXTERNAL_MPV_DIR\lib"
}
