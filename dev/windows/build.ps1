# Build jellium-desktop on Windows
# Must be run from Visual Studio Developer Command Prompt or with vcvars64.bat loaded

param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$BuildDir = Join-Path $RepoRoot "build"

. (Join-Path $PSScriptRoot "env.ps1")

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..."
    Remove-Item -Recurse -Force $BuildDir
}

# Locate mpv install (prefer mpv-install from build_mpv_source.ps1)
$XtaskArgs = @("xtask", "build")
$MpvInstallDir = Join-Path $RepoRoot "third_party\mpv-install"
$MpvDir = Join-Path $RepoRoot "third_party\mpv"
if (Test-Path (Join-Path $MpvInstallDir "lib\mpv.lib")) {
    $XtaskArgs += "--external-mpv=$MpvInstallDir"
} elseif (Test-Path (Join-Path $MpvDir "lib\mpv.lib")) {
    $XtaskArgs += "--external-mpv=$MpvDir"
}

Push-Location $RepoRoot
& cargo @XtaskArgs
$BuildResult = $LASTEXITCODE
Pop-Location

if ($BuildResult -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit $BuildResult
}

Write-Host ""
Write-Host "Build complete!" -ForegroundColor Green
Write-Host "Executable: $BuildDir\jellium-desktop.exe"
