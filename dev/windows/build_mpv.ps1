# Setup mpv import library for Windows
# Uses MSYS2-installed mpv or source-built mpv from build_mpv_source.ps1
#
# For MSYS2: Install mpv in MSYS2/CLANG64 environment:
#   pacman -S mingw-w64-clang-x86_64-mpv
#
# For source build: Run build_mpv_source.ps1 first

param(
    [string]$MsysPath = "C:\msys64\clang64",
    [switch]$UseMsys,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$OutputDir = Join-Path $RepoRoot "third_party\mpv-install"

# Check for MSVC environment (needed for lib.exe)
$HasMsvc = $false
if ($env:VSINSTALLDIR -and (Get-Command lib.exe -ErrorAction SilentlyContinue)) {
    $HasMsvc = $true
} else {
    # Try to load VS environment
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
            if (Get-Command lib.exe -ErrorAction SilentlyContinue) {
                $HasMsvc = $true
            }
        }
    }
}

# Determine source
$MpvDll = $null
$MpvInclude = $null
$MsysBinDir = $null

if ($UseMsys -or (Test-Path (Join-Path $MsysPath "bin\libmpv-2.dll"))) {
    # Use MSYS2 mpv
    $MsysBin = Join-Path $MsysPath "bin"
    $MpvDll = Join-Path $MsysBin "libmpv-2.dll"
    $MpvInclude = Join-Path $MsysPath "include\mpv"
    $MsysBinDir = $MsysBin

    if (-not (Test-Path $MpvDll)) {
        Write-Host "MSYS2 mpv not found at $MsysPath" -ForegroundColor Red
        Write-Host "Install with: pacman -S mingw-w64-clang-x86_64-mpv"
        exit 1
    }
    Write-Host "Using MSYS2 mpv from $MsysPath" -ForegroundColor Cyan
} else {
    # Check for mpv-install from build_mpv_source.ps1
    $MpvInstall = Join-Path $RepoRoot "third_party\mpv-install"
    $MpvInstallDll = Join-Path $MpvInstall "lib\libmpv-2.dll"

    if (Test-Path $MpvInstallDll) {
        $MpvDll = $MpvInstallDll
        # Prefer fork headers for gpu-next support
        $ForkHeaders = Join-Path $RepoRoot "third_party\mpv\include\mpv"
        if (Test-Path $ForkHeaders) {
            $MpvInclude = $ForkHeaders
        } else {
            $MpvInclude = Join-Path $MpvInstall "include\mpv"
        }
        Write-Host "Using source-built mpv from mpv-install" -ForegroundColor Cyan
    } else {
        Write-Host "mpv not found. Options:" -ForegroundColor Yellow
        Write-Host "  1. Run build_mpv_source.ps1 to build mpv from source"
        Write-Host "  2. Use -UseMsys to use MSYS2-installed mpv"
        Write-Host "     (install with: pacman -S mingw-w64-clang-x86_64-mpv)"
        exit 1
    }
}

# Check if already setup
$OutputLib = Join-Path $OutputDir "lib\mpv.lib"
if ((Test-Path $OutputLib) -and -not $Force) {
    Write-Host "mpv already setup at $OutputDir" -ForegroundColor Green
    Write-Host "Use -Force to rebuild"
    exit 0
}

Write-Host "Setting up mpv for MSVC..." -ForegroundColor Cyan

# Create output directory structure
if (Test-Path $OutputDir) {
    Remove-Item -Recurse -Force $OutputDir
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$LibDir = Join-Path $OutputDir "lib"
$IncludeDir = Join-Path $OutputDir "include"
New-Item -ItemType Directory -Path $LibDir -Force | Out-Null
New-Item -ItemType Directory -Path $IncludeDir -Force | Out-Null

# Copy include files
Write-Host "Copying headers..."
Copy-Item $MpvInclude (Join-Path $IncludeDir "mpv") -Recurse

# Copy DLL
Write-Host "Copying libmpv-2.dll..."
Copy-Item $MpvDll $LibDir

# Generate .def file using dumpbin or gendef
$DefFile = Join-Path $LibDir "libmpv-2.def"
Write-Host "Generating export definitions..."

if ($HasMsvc) {
    # Use dumpbin to extract exports
    $DllPath = Join-Path $LibDir "libmpv-2.dll"
    $Exports = & dumpbin /exports $DllPath | Where-Object { $_ -match "^\s+\d+\s+[A-F0-9]+\s+[A-F0-9]+\s+(\w+)" } | ForEach-Object {
        if ($_ -match "^\s+\d+\s+[A-F0-9]+\s+[A-F0-9]+\s+(\w+)") { $matches[1] }
    }

    if ($Exports.Count -gt 0) {
        $DefContent = "LIBRARY libmpv-2`nEXPORTS`n"
        $Exports | ForEach-Object { $DefContent += "    $_`n" }
        Set-Content -Path $DefFile -Value $DefContent
        Write-Host "Generated $DefFile with $($Exports.Count) exports"
    }
} elseif (Get-Command gendef -ErrorAction SilentlyContinue) {
    # Use gendef from MSYS2
    Push-Location $LibDir
    & gendef libmpv-2.dll
    Pop-Location
}

# Generate import library
if ($HasMsvc -and (Test-Path $DefFile)) {
    Write-Host "Generating MSVC import library..."
    Push-Location $LibDir
    & lib.exe /def:libmpv-2.def /out:mpv.lib /MACHINE:X64 2>&1 | Out-Null
    Pop-Location

    if (Test-Path $OutputLib) {
        Write-Host "Generated $OutputLib" -ForegroundColor Green
    } else {
        Write-Host "Failed to generate import library" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "Cannot generate import library - need MSVC environment" -ForegroundColor Yellow
    Write-Host "Run from 'x64 Native Tools Command Prompt for VS 2022'"
    exit 1
}

# If using MSYS2, note the DLL dependencies
if ($MsysBinDir) {
    Write-Host ""
    Write-Host "MSYS2 mpv dependencies:" -ForegroundColor Yellow
    Write-Host "When distributing, copy all required DLLs from:"
    Write-Host "  $MsysBinDir"
    Write-Host ""
    Write-Host "Quick copy command for dist folder:"
    Write-Host "  copy `"$MsysBinDir\*.dll`" <dist_folder>"
}

Write-Host ""
Write-Host "mpv setup complete!" -ForegroundColor Green
Write-Host "Output: $OutputDir"
Write-Host ""
Write-Host "Contents:"
Get-ChildItem $OutputDir -Recurse | ForEach-Object {
    $RelPath = $_.FullName.Substring($OutputDir.Length + 1)
    Write-Host "  $RelPath"
}
