# Build jellyfin-desktop on Windows
# Must be run from Visual Studio Developer Command Prompt or with vcvars64.bat loaded

param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "RelWithDebInfo",
    [switch]$Clean,
    [switch]$Configure
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$BuildDir = Join-Path $RepoRoot "build"

# Check for MSVC environment
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
        # mingw's libclang doesn't auto-pick the MSVC target triple, so pin it
        # so MSVC's arch-specific headers parse against the right default.
        if (-not $env:BINDGEN_EXTRA_CLANG_ARGS) {
            $env:BINDGEN_EXTRA_CLANG_ARGS = "--target=$Triple"
        }
        # Append (not prepend) so MSVC compilers from vcvars stay primary.
        $env:PATH = "$env:PATH;$MsysBin"
    } else {
        Write-Host "libclang.dll not found at $MsysBin" -ForegroundColor Red
        Write-Host "Run 'just deps' or install mingw-w64-clang-*-llvm via MSYS2 pacman."
        exit 1
    }
}

# Clean if requested
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..."
    Remove-Item -Recurse -Force $BuildDir
}

# Configure
if ($Configure -or -not (Test-Path (Join-Path $BuildDir "build.ninja"))) {
    Write-Host "Configuring with CMake..."

    $CmakeArgs = @(
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType"
    )

    # Add mpv paths (prefer mpv-install from build_mpv_source.ps1)
    $MpvInstallDir = Join-Path $RepoRoot "third_party\mpv-install"
    $MpvDir = Join-Path $RepoRoot "third_party\mpv"
    if (Test-Path (Join-Path $MpvInstallDir "lib\mpv.lib")) {
        $CmakeArgs += "-DEXTERNAL_MPV_DIR=$MpvInstallDir"
    } elseif (Test-Path (Join-Path $MpvDir "lib\mpv.lib")) {
        $CmakeArgs += "-DEXTERNAL_MPV_DIR=$MpvDir"
    }

    Push-Location $RepoRoot
    & cmake @CmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    Pop-Location
}

# Build
Write-Host "Building..."
Push-Location $BuildDir
& ninja
$BuildResult = $LASTEXITCODE
Pop-Location

if ($BuildResult -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit $BuildResult
}

Write-Host ""
Write-Host "Build complete!" -ForegroundColor Green
Write-Host "Executable: $BuildDir\jellyfin-desktop.exe"
