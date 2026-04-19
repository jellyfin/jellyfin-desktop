# Build CEF wrapper library for Windows
# Must be run from Visual Studio Developer Command Prompt or after vcvars64.bat

param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$CefDir = Join-Path $RepoRoot "third_party\cef"

# Check for CEF
if (-not (Test-Path $CefDir)) {
    Write-Host "CEF not found at $CefDir" -ForegroundColor Red
    Write-Host "Run setup.ps1 or dev\tools\download_cef.py first"
    exit 1
}

# Check for MSVC environment
if (-not $env:VSINSTALLDIR) {
    Write-Host "MSVC environment not detected." -ForegroundColor Yellow
    Write-Host "Attempting to load vcvars64.bat..."

    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $VsPath = & $VsWhere -latest -products * -property installationPath
        $VcVars = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $VcVars) {
            # Load VS environment
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

# Clean if requested
if ($Clean) {
    Write-Host "Cleaning CEF build..."
    $FilesToRemove = @(
        "CMakeCache.txt", "CMakeFiles", "cmake_install.cmake",
        "build.ninja", ".ninja_deps", ".ninja_log",
        "libcef_dll_wrapper", "tests"
    )
    foreach ($f in $FilesToRemove) {
        $Path = Join-Path $CefDir $f
        if (Test-Path $Path) {
            Remove-Item -Recurse -Force $Path
        }
    }
}

# Check if already built (Ninja outputs to libcef_dll_wrapper/)
$WrapperLib = Join-Path $CefDir "libcef_dll_wrapper\libcef_dll_wrapper.lib"
if ((Test-Path $WrapperLib) -and -not $Clean) {
    Write-Host "CEF wrapper already built at $WrapperLib" -ForegroundColor Green
    Write-Host "Use -Clean to rebuild"
    exit 0
}

Write-Host "Building CEF wrapper library ($BuildType)..." -ForegroundColor Cyan

# Configure
Push-Location $CefDir
& cmake -G Ninja -DCMAKE_BUILD_TYPE=$BuildType .
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    throw "CMake configure failed"
}

# Build just the wrapper
& ninja libcef_dll_wrapper
$BuildResult = $LASTEXITCODE
Pop-Location

if ($BuildResult -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit $BuildResult
}

$BuiltLib = Join-Path $CefDir "libcef_dll_wrapper\libcef_dll_wrapper.lib"
if (Test-Path $BuiltLib) {
    Write-Host ""
    Write-Host "CEF wrapper built successfully!" -ForegroundColor Green
    Write-Host "Library: $BuiltLib"
} else {
    Write-Host "Build completed but library not found at expected location" -ForegroundColor Yellow
    Get-ChildItem $CefDir -Recurse -Filter "libcef_dll_wrapper.lib" | ForEach-Object {
        Write-Host "  Found: $($_.FullName)"
    }
}
