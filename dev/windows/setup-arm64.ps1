$ErrorActionPreference = 'Stop'
if ($env:HOST_ARCH -ne 'ARM64' -or -not $env:DEPS_DIR -or -not $env:VCToolsRedistDir) {
    throw 'Run dev\windows\setup.bat on Windows ARM64 with the native MSVC ARM64 build tools installed.'
}

$deps = $env:DEPS_DIR
$qtRoot = $env:QTROOT_WIN
New-Item -ItemType Directory -Force $deps | Out-Null

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw 'Install CMake (or the Visual Studio CMake tools component), then rerun setup.'
}

$sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (-not (Test-Path -LiteralPath $sevenZip)) {
    $command = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($command) { $sevenZip = $command.Source }
    else {
        $sevenZip = Join-Path $deps 'tools\7zr.exe'
        if (-not (Test-Path -LiteralPath $sevenZip)) {
            New-Item -ItemType Directory -Force (Split-Path $sevenZip) | Out-Null
            Invoke-WebRequest -UseBasicParsing 'https://www.7-zip.org/a/7zr.exe' -OutFile $sevenZip
        }
    }
}
if (-not (Test-Path -LiteralPath $sevenZip)) { throw '7-Zip was not found after installation.' }

$python = Join-Path $deps 'tools\python\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    & python -m venv (Join-Path $deps 'tools\python')
    if ($LASTEXITCODE -ne 0) { throw 'Install Python with python on PATH, then rerun setup.' }
}
& $python -c "import importlib.util, sys; sys.exit(importlib.util.find_spec('aqt') is None)"
if ($LASTEXITCODE -ne 0) {
    & $python -m pip install --quiet aqtinstall==3.3.0
    if ($LASTEXITCODE -ne 0) { throw 'aqtinstall installation failed.' }
}

$requiredModules = 'Core', 'WebChannel', 'Positioning', 'TaskTree'
$missingModules = @($requiredModules | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $qtRoot "lib\cmake\Qt6$_\Qt6$($_)Config.cmake"))
})
if ($missingModules.Count) {
    Push-Location $deps
    try {
        & $python -m aqt install-qt windows_arm64 desktop $env:QT_VERSION win64_msvc2022_arm64 `
            -m qtwebchannel qtpositioning qttasktree -O (Join-Path $deps 'qt')
        if ($LASTEXITCODE -ne 0) { throw 'Qt ARM64 installation failed.' }
    }
    finally { Pop-Location }
}
& "$PSScriptRoot\install-qtwebengine-arm64.ps1" -Version $env:QT_VERSION `
    -QtDirectory (Join-Path $deps 'qt') -SevenZip $sevenZip
if ($LASTEXITCODE -ne 0) { throw 'Qt WebEngine ARM64 installation failed.' }

# The SourceForge aarch64 builds disable OpenGL; use mpv's stable native render API.
$mpv = Join-Path $deps 'mpv'
$versionFile = Join-Path $mpv 'version.txt'
$installedVersion = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -Raw).Trim() }
if ($installedVersion -ne $env:MPV_ARM64_VERSION -or
    -not (Test-Path -LiteralPath (Join-Path $mpv 'include\mpv\render_d3d11.h')) -or
    -not (Test-Path -LiteralPath (Join-Path $mpv 'libmpv-2.dll'))) {
    $archive = Join-Path $deps 'libmpv-arm64.zip'
    $version = $env:MPV_ARM64_VERSION
    Invoke-WebRequest -UseBasicParsing `
        "https://github.com/mpv-player/mpv/releases/download/v$version/libmpv-v$version-aarch64-w64-mingw32-gpl.zip" `
        -OutFile $archive
    $stream = [IO.File]::OpenRead($archive)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $sha256.Dispose() }
    if ($hash -ne $env:MPV_ARM64_SHA256) {
        throw 'ARM64 libmpv archive checksum mismatch.'
    }
    & $python -m zipfile -e $archive $mpv
    if ($LASTEXITCODE -ne 0) { throw 'ARM64 libmpv ZIP extraction failed.' }
    $importLibrary = Join-Path $mpv 'libmpv-2.dll.lib'
    if (Test-Path -LiteralPath $importLibrary) { Remove-Item -LiteralPath $importLibrary }
    Set-Content -LiteralPath $versionFile -Value $version -Encoding ascii
    Remove-Item -LiteralPath $archive
}

$runtime = Get-ChildItem (Join-Path $env:VCToolsRedistDir 'arm64\Microsoft.VC*.CRT') -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $runtime) { throw 'ARM64 MSVC runtime DLLs are missing from the Visual Studio installation.' }
$runtimeDir = Join-Path $deps 'vcruntime'
New-Item -ItemType Directory -Force $runtimeDir | Out-Null
Copy-Item (Join-Path $runtime.FullName '*.dll') $runtimeDir
$redist = Join-Path $deps 'vc_redist.arm64.exe'
if (-not (Test-Path -LiteralPath $redist)) {
    Invoke-WebRequest -UseBasicParsing 'https://aka.ms/vs/17/release/vc_redist.arm64.exe' -OutFile $redist
}

$presets = (Get-Content "$PSScriptRoot\..\CMakePresets.json.in" -Raw).
    Replace('@QT_VERSION@', $env:QT_VERSION).Replace('@BREW_PREFIX@', '') | ConvertFrom-Json
$windows = $presets.configurePresets | Where-Object name -eq 'windows-dev'
$windows.binaryDir = '${sourceDir}/build/arm64'
$windows.cacheVariables.QTROOT = $qtRoot.Replace('\', '/')
$windows.cacheVariables.MPV_INCLUDE_DIR = "$mpv\include".Replace('\', '/')
$windows.cacheVariables.MPV_LIBRARY = "$mpv\libmpv-2.dll.lib".Replace('\', '/')
$windows.cacheVariables | Add-Member -NotePropertyName WINDOWS_DEPENDENCY_DIR -NotePropertyValue $deps.Replace('\', '/')
$presets | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath "$env:PROJECT_ROOT\CMakePresets.json"

Write-Host 'ARM64 dependencies are ready. Run dev\windows\build.bat.'
Write-Host 'For installer/ZIP packaging, also install Inno Setup 6 (winget install JRSoftware.InnoSetup).'
