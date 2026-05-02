# Build mpv from submodule source using MSYS2 for dependencies
# Produces the custom mpv fork (with gpu-next/Vulkan support) as a Windows DLL
#
# Prerequisites: MSYS2 installed (https://www.msys2.org/)
# Dependencies are installed automatically via pacman.

param(
    [string]$MsysPath = "C:\msys64",
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$OutputDir = Join-Path $RepoRoot "third_party\mpv-install"
$MpvSourceDir = Join-Path $RepoRoot "third_party\mpv"

# MSYS2 environment based on target architecture
if ($Arch -eq "arm64") {
    $MsysEnv = "CLANGARM64"
    $PkgPrefix = "mingw-w64-clang-aarch64"
    $LibMachine = "ARM64"
} else {
    $MsysEnv = "CLANG64"
    $PkgPrefix = "mingw-w64-clang-x86_64"
    $LibMachine = "X64"
}

# Check if already built
$OutputLib = Join-Path $OutputDir "lib\mpv.lib"
if ((Test-Path $OutputLib) -and -not $Force) {
    Write-Host "mpv already built at $OutputDir" -ForegroundColor Green
    Write-Host "Use -Force to rebuild"
    exit 0
}

# Verify mpv submodule exists
if (-not (Test-Path (Join-Path $MpvSourceDir "meson.build"))) {
    Write-Host "mpv submodule not found. Run: git submodule update --init --recursive" -ForegroundColor Red
    exit 1
}

# Check for MSYS2, install if missing
$MsysBash = Join-Path $MsysPath "usr\bin\bash.exe"
if (-not (Test-Path $MsysBash)) {
    Write-Host "MSYS2 not found at $MsysPath, installing..." -ForegroundColor Yellow

    $MsysInstaller = Join-Path $env:TEMP "msys2-installer.exe"
    $MsysUrl = "https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.sfx.exe"
    Write-Host "Downloading MSYS2..."
    & curl.exe -L -o $MsysInstaller $MsysUrl
    if ($LASTEXITCODE -ne 0) { throw "Failed to download MSYS2" }

    Write-Host "Extracting MSYS2 to C:\..."
    & $MsysInstaller -y "-oC:\"
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract MSYS2" }
    Remove-Item $MsysInstaller -ErrorAction SilentlyContinue

    # Initialize MSYS2 (first run triggers setup)
    Write-Host "Initializing MSYS2..."
    $env:MSYSTEM = "MSYS"
    $env:CHERE_INVOKING = "1"
    & $MsysBash -l -c "pacman-key --init && pacman -Syu --noconfirm"

    if (-not (Test-Path $MsysBash)) {
        Write-Host "MSYS2 installation failed" -ForegroundColor Red
        exit 1
    }
    Write-Host "MSYS2 installed" -ForegroundColor Green
}

Write-Host "=== Building mpv from submodule ===" -ForegroundColor Cyan
Write-Host "MSYS2: $MsysPath ($MsysEnv)"
Write-Host "Source: $MpvSourceDir"
Write-Host ""

# Convert Windows path to MSYS2 path (C:\foo\bar -> /c/foo/bar)
function ConvertTo-MsysPath($WinPath) {
    $Resolved = (Resolve-Path $WinPath).Path -replace '\\', '/'
    if ($Resolved -match '^([A-Za-z]):(.*)') {
        '/' + $matches[1].ToLower() + $matches[2]
    } else {
        $Resolved
    }
}

$MsysMpvSource = ConvertTo-MsysPath $MpvSourceDir

# Run a command in MSYS2
function Invoke-Msys2 {
    param([string]$Command, [string]$Description)
    Write-Host "$Description..." -ForegroundColor Cyan
    $env:MSYSTEM = $MsysEnv
    $env:CHERE_INVOKING = "1"
    & $MsysBash -l -c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Failed: $Description"
    }
}

# Install build dependencies
Invoke-Msys2 @"
pacman -S --needed --noconfirm \
    $PkgPrefix-cc \
    $PkgPrefix-meson \
    $PkgPrefix-pkgconf \
    $PkgPrefix-ffmpeg \
    $PkgPrefix-libplacebo \
    $PkgPrefix-libass \
    $PkgPrefix-vulkan-headers \
    $PkgPrefix-vulkan-loader \
    $PkgPrefix-shaderc \
    $PkgPrefix-spirv-cross
"@ -Description "Installing MSYS2 dependencies"

# Clean previous build if forcing
$MesonBuildDir = Join-Path $MpvSourceDir "build"
if ($Force -and (Test-Path $MesonBuildDir)) {
    Write-Host "Cleaning previous build..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $MesonBuildDir
}

# Configure with meson
if (-not (Test-Path (Join-Path $MesonBuildDir "build.ninja"))) {
    Invoke-Msys2 @"
cd '$MsysMpvSource' && \
meson setup build --default-library=shared \
    -Dlibmpv=true \
    -Dcplayer=true \
    -Dlua=disabled \
    -Djavascript=disabled \
    -Dcdda=disabled \
    -Ddvdnav=disabled \
    -Dlibbluray=disabled \
    -Dlibarchive=disabled \
    -Drubberband=disabled \
    -Dvapoursynth=disabled
"@ -Description "Configuring mpv with meson"
} else {
    Write-Host "Meson already configured (use -Force to reconfigure)" -ForegroundColor Yellow
}

# Build
Invoke-Msys2 "cd '$MsysMpvSource' && meson compile -C build" -Description "Building mpv"

# Verify the DLL was produced
$BuiltDll = Join-Path $MesonBuildDir "libmpv-2.dll"
if (-not (Test-Path $BuiltDll)) {
    Write-Host "Build succeeded but libmpv-2.dll not found" -ForegroundColor Red
    Get-ChildItem $MesonBuildDir -Filter "*.dll" -Recurse | ForEach-Object {
        Write-Host "  Found: $($_.FullName)"
    }
    exit 1
}

Write-Host ""
Write-Host "=== Setting up output directory ===" -ForegroundColor Cyan

# Setup output directory (matches EXTERNAL_MPV_DIR layout)
if (Test-Path $OutputDir) {
    Remove-Item -Recurse -Force $OutputDir
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$LibDir = Join-Path $OutputDir "lib"
$IncludeDir = Join-Path $OutputDir "include"
New-Item -ItemType Directory -Path $LibDir -Force | Out-Null
New-Item -ItemType Directory -Path $IncludeDir -Force | Out-Null

# Copy headers from submodule fork (includes render_vk.h for gpu-next)
Write-Host "Copying headers..."
Copy-Item (Join-Path $MpvSourceDir "include\mpv") (Join-Path $IncludeDir "mpv") -Recurse

# Copy DLL
Write-Host "Copying libmpv-2.dll..."
Copy-Item $BuiltDll $LibDir

# Generate MSVC import library
Write-Host "Generating MSVC import library..."

$HasMsvc = $false
if ($env:VSINSTALLDIR -and (Get-Command lib.exe -ErrorAction SilentlyContinue)) {
    $HasMsvc = $true
} else {
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

if ($HasMsvc) {
    # Use dumpbin + lib.exe (best quality import lib)
    $DllPath = Join-Path $LibDir "libmpv-2.dll"
    $DefFile = Join-Path $LibDir "libmpv-2.def"

    $DumpOutput = & dumpbin /exports $DllPath
    $Exports = $DumpOutput | Where-Object {
        $_ -match "^\s+\d+\s+[A-F0-9]+\s+[A-F0-9]+\s+(\w+)"
    } | ForEach-Object {
        if ($_ -match "^\s+\d+\s+[A-F0-9]+\s+[A-F0-9]+\s+(\w+)") { $matches[1] }
    }

    if ($Exports.Count -gt 0) {
        $DefContent = "LIBRARY libmpv-2`nEXPORTS`n"
        $Exports | ForEach-Object { $DefContent += "    $_`n" }
        Set-Content -Path $DefFile -Value $DefContent

        Push-Location $LibDir
        & lib.exe /def:libmpv-2.def /out:mpv.lib /MACHINE:$LibMachine 2>&1 | Out-Null
        Pop-Location

        if (Test-Path $OutputLib) {
            Write-Host "Generated mpv.lib ($($Exports.Count) exports)" -ForegroundColor Green
        } else {
            Write-Host "lib.exe failed to generate import library" -ForegroundColor Red
            exit 1
        }
    } else {
        Write-Host "No exports found in DLL" -ForegroundColor Red
        exit 1
    }
} else {
    # Fallback: use gendef + dlltool from MSYS2
    Write-Host "MSVC not available, using MSYS2 tools..." -ForegroundColor Yellow
    $MsysLibDir = ConvertTo-MsysPath $LibDir
    Invoke-Msys2 "cd '$MsysLibDir' && gendef libmpv-2.dll && dlltool -d libmpv-2.def -l mpv.lib" `
        -Description "Generating import library with dlltool"

    if (Test-Path $OutputLib) {
        Write-Host "Generated mpv.lib (via dlltool)" -ForegroundColor Green
    } else {
        Write-Host "Failed to generate import library" -ForegroundColor Red
        exit 1
    }
}

# Collect runtime DLL dependencies recursively from MSYS2
Write-Host ""
Write-Host "=== Collecting runtime dependencies ===" -ForegroundColor Cyan
$MsysBinDir = Join-Path $MsysPath "$MsysEnv\bin"
$MsysEnvLower = $MsysEnv.ToLower()
$MsysLibDir = ConvertTo-MsysPath $LibDir

# Write a helper script to resolve deps recursively, then run it
$DepScript = @"
#!/bin/bash
MSYS_BIN=/$MsysEnvLower/bin
OUT_DIR='$MsysLibDir'
declare -A seen

resolve_deps() {
    local dll=`"`$1`"
    local path=`"`$2`"
    [ -n `"`${seen[`$dll]}`" ] && return
    seen[`$dll]=1
    objdump -p `"`$path`" 2>/dev/null | grep 'DLL Name' | awk '{print `$3}' | while read dep; do
        if [ -f `"`$MSYS_BIN/`$dep`" ] && [ -z `"`${seen[`$dep]}`" ]; then
            cp -v `"`$MSYS_BIN/`$dep`" `"`$OUT_DIR/`"
            resolve_deps `"`$dep`" `"`$MSYS_BIN/`$dep`"
        fi
    done
}

resolve_deps libmpv-2.dll `"`$OUT_DIR/libmpv-2.dll`"
"@

$DepScriptPath = Join-Path $MesonBuildDir "resolve_deps.sh"
Set-Content -Path $DepScriptPath -Value $DepScript -NoNewline
$MsysDepScript = ConvertTo-MsysPath $DepScriptPath

Invoke-Msys2 "bash '$MsysDepScript'" -Description "Copying MSYS2 runtime dependencies"

# Count what we copied
$DllCount = (Get-ChildItem $LibDir -Filter "*.dll").Count
Write-Host "Collected $DllCount DLLs total" -ForegroundColor Green

Write-Host ""
Write-Host "=== Build complete ===" -ForegroundColor Green
Write-Host "Output: $OutputDir"
Write-Host ""
Write-Host "Contents:"
Get-ChildItem $OutputDir -Recurse -File | ForEach-Object {
    Write-Host "  $($_.FullName.Substring($OutputDir.Length + 1))"
}
