# Setup Windows development environment for jellyfin-desktop-cef
# Prerequisites: Visual Studio 2022, Python 3, 7-Zip, CMake, Ninja

param(
    [switch]$SkipMpv,
    [switch]$SkipCef,
    [switch]$SkipSdl,
    [switch]$SkipVulkan
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName

Write-Host "=== jellyfin-desktop-cef Windows Setup ===" -ForegroundColor Cyan
Write-Host "Repository: $RepoRoot"
Write-Host ""

# Check prerequisites
function Test-Command($Command) {
    return [bool](Get-Command $Command -ErrorAction SilentlyContinue)
}

$Missing = @()
if (-not (Test-Command "python")) { $Missing += "Python 3" }
if (-not (Test-Command "cmake")) { $Missing += "CMake" }
if (-not (Test-Command "ninja")) { $Missing += "Ninja" }
if (-not (Test-Command "7z")) { $Missing += "7-Zip" }

if ($Missing.Count -gt 0) {
    Write-Host "Missing prerequisites:" -ForegroundColor Red
    $Missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Install via winget or scoop:"
    Write-Host "  winget install Python.Python.3.12"
    Write-Host "  winget install Kitware.CMake"
    Write-Host "  winget install Ninja-build.Ninja"
    Write-Host "  winget install 7zip.7zip"
    exit 1
}

# Check for Visual Studio
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    Write-Host "Visual Studio not found. Install Visual Studio 2022 with C++ workload." -ForegroundColor Red
    exit 1
}

$VsPath = & $VsWhere -latest -products * -property installationPath
if (-not $VsPath) {
    Write-Host "Visual Studio installation not found." -ForegroundColor Red
    exit 1
}
Write-Host "Visual Studio: $VsPath" -ForegroundColor Green

# Initialize git submodules
Write-Host ""
Write-Host "=== Git Submodules ===" -ForegroundColor Cyan
Push-Location $RepoRoot
& git submodule update --init --recursive
Pop-Location

# Download CEF
if (-not $SkipCef) {
    Write-Host ""
    Write-Host "=== CEF (Chromium Embedded Framework) ===" -ForegroundColor Cyan
    & python (Join-Path $RepoRoot "dev\download_cef.py")
}

# Download libmpv
if (-not $SkipMpv) {
    Write-Host ""
    Write-Host "=== libmpv ===" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "download_mpv.ps1")
}

# SDL3 setup - download prebuilt VC package
if (-not $SkipSdl) {
    Write-Host ""
    Write-Host "=== SDL3 ===" -ForegroundColor Cyan
    $SdlDir = Join-Path $RepoRoot "third_party\SDL"
    if ((Test-Path (Join-Path $SdlDir "cmake")) -and (Test-Path (Join-Path $SdlDir "lib"))) {
        Write-Host "SDL3 already set up at $SdlDir" -ForegroundColor Green
    } else {
        $SdlVersion = "3.4.0"
        $SdlUrl = "https://github.com/libsdl-org/SDL/releases/download/release-$SdlVersion/SDL3-devel-$SdlVersion-VC.zip"
        $SdlZip = Join-Path $RepoRoot "third_party\SDL3-VC.zip"
        $SdlExtracted = Join-Path $RepoRoot "third_party\SDL3-$SdlVersion"

        Write-Host "Downloading SDL3 $SdlVersion..."
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $SdlUrl -OutFile $SdlZip -UseBasicParsing
        $ProgressPreference = 'Continue'

        Write-Host "Extracting..."
        & 7z x $SdlZip -o"$(Join-Path $RepoRoot "third_party")" -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "SDL3 extraction failed" }

        # Rename to third_party/SDL
        if (Test-Path $SdlDir) { Remove-Item -Recurse -Force $SdlDir }
        Rename-Item $SdlExtracted $SdlDir

        # Clean up zip
        Remove-Item $SdlZip -ErrorAction SilentlyContinue

        Write-Host "SDL3 installed to $SdlDir" -ForegroundColor Green
    }
}

# Vulkan SDK check
if (-not $SkipVulkan) {
    Write-Host ""
    Write-Host "=== Vulkan SDK ===" -ForegroundColor Cyan
    $VulkanSdk = $env:VULKAN_SDK
    if ($VulkanSdk -and (Test-Path $VulkanSdk)) {
        Write-Host "Vulkan SDK: $VulkanSdk" -ForegroundColor Green
    } else {
        Write-Host "Vulkan SDK not found." -ForegroundColor Yellow
        Write-Host "Download from: https://vulkan.lunarg.com/sdk/home"
        Write-Host "Or install via: winget install KhronosGroup.VulkanSDK"
    }
}

Write-Host ""
Write-Host "=== Setup Complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "To build:"
Write-Host "  1. Open 'x64 Native Tools Command Prompt for VS 2022'"
Write-Host "  2. Navigate to repository: cd $RepoRoot"
Write-Host "  3. Configure:"
Write-Host "     cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
Write-Host "  4. Build:"
Write-Host "     cmake --build build"
Write-Host ""
Write-Host "Or use dev\windows\build.ps1 for a complete build"
