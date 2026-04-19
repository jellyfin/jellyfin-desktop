# Setup Windows development environment for jellyfin-desktop
# Prerequisites: Visual Studio 2022, Python 3, 7-Zip, CMake, Ninja

param(
    [switch]$SkipMpv,
    [switch]$SkipCef
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName

# Refresh PATH to pick up recently installed tools (e.g. via winget)
$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path', 'User')

Write-Host "=== jellyfin-desktop Windows Setup ===" -ForegroundColor Cyan
Write-Host "Repository: $RepoRoot"
Write-Host ""

# Check prerequisites, auto-installing missing ones via winget
function Test-Command($Command) {
    return [bool](Get-Command $Command -ErrorAction SilentlyContinue)
}

$Prerequisites = @(
    @{ Command = "python"; Name = "Python 3";  WingetId = "Python.Python.3.12" },
    @{ Command = "cmake";  Name = "CMake";     WingetId = "Kitware.CMake" },
    @{ Command = "ninja";  Name = "Ninja";     WingetId = "Ninja-build.Ninja" },
    @{ Command = "7z";     Name = "7-Zip";     WingetId = "7zip.7zip" },
    @{ Command = "git";    Name = "Git";       WingetId = "Git.Git" }
)

function Find-Command($Command) {
    # Check PATH first
    if (Test-Command $Command) { return $true }
    # Some installers (e.g. 7-Zip) don't add to PATH - search Program Files
    $Exe = Get-ChildItem -Path "$env:ProgramFiles", "${env:ProgramFiles(x86)}", "$env:LocalAppData" `
        -Filter "$Command.exe" -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($Exe) {
        $env:Path += ";$($Exe.DirectoryName)"
        return $true
    }
    return $false
}

foreach ($Prereq in $Prerequisites) {
    if (-not (Find-Command $Prereq.Command)) {
        Write-Host "$($Prereq.Name) not found, installing via winget..." -ForegroundColor Yellow
        & winget install --source winget --accept-package-agreements --accept-source-agreements $Prereq.WingetId
        # Refresh PATH after install
        $env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path', 'User')
        if (-not (Find-Command $Prereq.Command)) {
            Write-Host "$($Prereq.Name) installed but not found in PATH. Restart your shell and re-run." -ForegroundColor Red
            exit 1
        }
        Write-Host "$($Prereq.Name) installed" -ForegroundColor Green
    }
}

# Check for Visual Studio with C++ workload
Write-Host "=== Visual Studio ===" -ForegroundColor Cyan
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

# Check if VS with VC tools is already installed
$VsPath = $null
if (Test-Path $VsWhere) {
    $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

if (-not $VsPath) {
    Write-Host "Visual Studio C++ workload not found, installing Build Tools via winget..." -ForegroundColor Yellow
    & winget install --source winget --accept-package-agreements --accept-source-agreements --force Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
        Write-Host "Failed to install Visual Studio Build Tools" -ForegroundColor Red
        exit 1
    }
    # Re-check
    $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsPath) {
        Write-Host "Visual Studio C++ workload still not found after install." -ForegroundColor Red
        exit 1
    }
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
    & python (Join-Path $RepoRoot "dev\tools\download_cef.py")
}

# Build libmpv from source
if (-not $SkipMpv) {
    Write-Host ""
    Write-Host "=== libmpv ===" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build_mpv_source.ps1")
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
