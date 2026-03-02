# Download and setup libmpv for Windows development
# Downloads prebuilt libmpv DLL and uses headers from the mpv submodule fork
# (which includes render_vk.h for gpu-next support)

param(
    [string]$Version = "20251214-git-f7be2ee",
    [string]$Arch = "x86_64-v3",
    [string]$OutputDir = "third_party\mpv-install"
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$OutputPath = Join-Path $RepoRoot $OutputDir

# Check if already set up
$MpvLib = Join-Path $OutputPath "lib\mpv.lib"
if (Test-Path $MpvLib) {
    Write-Host "mpv already set up at $OutputPath" -ForegroundColor Green
    Write-Host "Delete $OutputPath to re-download"
    exit 0
}

Write-Host "Downloading libmpv $Version for $Arch..."

# Create temp directory
$TempDir = Join-Path $env:TEMP "mpv-download-$(Get-Random)"
New-Item -ItemType Directory -Path $TempDir -Force | Out-Null

try {
    # Download mpv dev package
    $Url = "https://sourceforge.net/projects/mpv-player-windows/files/libmpv/mpv-dev-$Arch-$Version.7z/download"
    $ArchivePath = Join-Path $TempDir "mpv.7z"

    Write-Host "Downloading from $Url"
    # Use curl.exe for SourceForge downloads (handles redirects reliably)
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        & curl.exe -L -o $ArchivePath $Url
        if ($LASTEXITCODE -ne 0) { throw "curl download failed" }
    } else {
        # Fallback to Invoke-WebRequest with redirect handling
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Url -OutFile $ArchivePath -UseBasicParsing -MaximumRedirection 10 -UserAgent "Mozilla/5.0"
    }

    # Verify download is a real archive (not an HTML redirect page)
    $FileSize = (Get-Item $ArchivePath).Length
    if ($FileSize -lt 1MB) {
        throw "Downloaded file too small ($FileSize bytes) - likely a redirect page, not the archive"
    }

    # Extract
    Write-Host "Extracting..."
    $ExtractDir = Join-Path $TempDir "extracted"
    & 7z x $ArchivePath -o"$ExtractDir" -y
    if ($LASTEXITCODE -ne 0) { throw "7z extraction failed" }

    # Setup output directory structure (matches EXTERNAL_MPV_DIR expectations)
    if (Test-Path $OutputPath) {
        Remove-Item -Recurse -Force $OutputPath
    }
    New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null
    $LibDir = Join-Path $OutputPath "lib"
    $IncludeDir = Join-Path $OutputPath "include"
    New-Item -ItemType Directory -Path $LibDir -Force | Out-Null
    New-Item -ItemType Directory -Path $IncludeDir -Force | Out-Null

    # Use headers from the mpv submodule fork (includes render_vk.h for gpu-next)
    $ForkHeaders = Join-Path $RepoRoot "third_party\mpv\include\mpv"
    if (Test-Path $ForkHeaders) {
        Write-Host "Using headers from mpv submodule fork (includes gpu-next support)"
        Copy-Item $ForkHeaders (Join-Path $IncludeDir "mpv") -Recurse
    } else {
        Write-Host "mpv submodule not found, using headers from download" -ForegroundColor Yellow
        $DownloadedInclude = Join-Path $ExtractDir "include"
        Copy-Item -Path "$DownloadedInclude\*" -Destination $IncludeDir -Recurse -Force
    }

    # Copy DLL to lib/
    Move-Item (Join-Path $ExtractDir "libmpv-2.dll") $LibDir

    # Copy import library if present
    $ImportLib = Join-Path $ExtractDir "libmpv.dll.a"
    if (Test-Path $ImportLib) {
        Move-Item $ImportLib (Join-Path $LibDir "libmpv.dll.a")
    }

    # Generate MSVC import library
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

    if ($HasMsvc) {
        Write-Host "Generating MSVC import library..."
        $DllPath = Join-Path $LibDir "libmpv-2.dll"

        # Extract exports using dumpbin
        $DumpOutput = & dumpbin /exports $DllPath
        $Exports = $DumpOutput | Where-Object {
            $_ -match "^\s+\d+\s+[A-F0-9]+\s+[A-F0-9]+\s+(\w+)"
        } | ForEach-Object {
            if ($_ -match "^\s+\d+\s+[A-F0-9]+\s+[A-F0-9]+\s+(\w+)") { $matches[1] }
        }

        if ($Exports.Count -gt 0) {
            $DefFile = Join-Path $LibDir "libmpv-2.def"
            $DefContent = "LIBRARY libmpv-2`nEXPORTS`n"
            $Exports | ForEach-Object { $DefContent += "    $_`n" }
            Set-Content -Path $DefFile -Value $DefContent
            Write-Host "Generated $DefFile with $($Exports.Count) exports"

            Push-Location $LibDir
            & lib.exe /def:libmpv-2.def /out:mpv.lib /MACHINE:X64 2>&1 | Out-Null
            Pop-Location

            if (Test-Path $MpvLib) {
                Write-Host "Generated $MpvLib" -ForegroundColor Green
            } else {
                Write-Host "Failed to generate import library" -ForegroundColor Red
                exit 1
            }
        } else {
            Write-Host "No exports found in DLL" -ForegroundColor Red
            exit 1
        }
    } else {
        Write-Host "MSVC not available - import library not generated" -ForegroundColor Yellow
        Write-Host "Run build_cef.ps1 or build_mpv.ps1 from VS Developer Prompt to generate it"
    }

    Write-Host ""
    Write-Host "mpv installed to $OutputPath" -ForegroundColor Green
    Write-Host ""
    Write-Host "Contents:"
    Get-ChildItem $OutputPath -Recurse | ForEach-Object {
        Write-Host "  $($_.FullName.Substring($OutputPath.Length + 1))"
    }
}
finally {
    # Cleanup
    if (Test-Path $TempDir) {
        Remove-Item -Recurse -Force $TempDir
    }
}
