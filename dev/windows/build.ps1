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
        $VsPath = & $VsWhere -latest -products * -property installationPath
        $VcVars = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $VcVars) {
            cmd /c "`"$VcVars`" && set" | ForEach-Object {
                if ($_ -match "^([^=]+)=(.*)$") {
                    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
                }
            }
            Write-Host "Loaded Visual Studio environment" -ForegroundColor Green
        }
    }

    if (-not $env:VSINSTALLDIR) {
        Write-Host "Could not load MSVC environment." -ForegroundColor Red
        Write-Host "Run from 'x64 Native Tools Command Prompt for VS 2022'"
        exit 1
    }
}

# Auto-detect Vulkan SDK if not set
if (-not $env:VULKAN_SDK) {
    $VulkanBase = "C:\VulkanSDK"
    if (Test-Path $VulkanBase) {
        $Latest = Get-ChildItem $VulkanBase -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($Latest) {
            $env:VULKAN_SDK = $Latest.FullName
            Write-Host "Detected Vulkan SDK: $($env:VULKAN_SDK)" -ForegroundColor Green
        }
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
Write-Host "Executable: $BuildDir\jellyfin-desktop.exe"
