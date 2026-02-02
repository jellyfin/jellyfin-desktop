# Build jellyfin-desktop-cef on Windows
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
    Write-Host "Run this script from 'x64 Native Tools Command Prompt for VS 2022'"
    Write-Host "Or run: "
    Write-Host '  & "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"'
    exit 1
}

# Clean if requested
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..."
    Remove-Item -Recurse -Force $BuildDir
}

# Configure
if ($Configure -or -not (Test-Path (Join-Path $BuildDir "build.ninja"))) {
    Write-Host "Configuring with CMake..."

    $MpvDir = Join-Path $RepoRoot "third_party\mpv"
    $CmakeArgs = @(
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType"
    )

    # Add mpv paths if downloaded
    if (Test-Path (Join-Path $MpvDir "include")) {
        $CmakeArgs += "-DEXTERNAL_MPV_DIR=$MpvDir"
    }

    # Add SDL3 paths if present
    $SdlDir = Join-Path $RepoRoot "third_party\SDL"
    if (Test-Path (Join-Path $SdlDir "cmake")) {
        $CmakeArgs += "-DSDL3_DIR=$SdlDir\cmake"
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
Write-Host "Executable: $BuildDir\jellyfin-desktop-cef.exe"
