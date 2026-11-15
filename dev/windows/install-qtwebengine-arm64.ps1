param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [Parameter(Mandatory = $true)]
    [string]$QtDirectory,
    [Parameter(Mandatory = $true)]
    [string]$SevenZip
)

$ErrorActionPreference = 'Stop'
$QtDirectory = [IO.Path]::GetFullPath($QtDirectory)
$qtRoot = Join-Path $QtDirectory "$Version\msvc2022_arm64"
$config = Join-Path $qtRoot 'lib\cmake\Qt6WebEngineCore\Qt6WebEngineCoreConfig.cmake'
if (Test-Path -LiteralPath $config) {
    Write-Host "Qt WebEngine $Version ARM64 is already installed."
    exit 0
}

# aqtinstall 3.3.0 omits the windows_arm64 WebEngine extension repository.
$versionId = $Version.Replace('.', '')
$repository = "https://download.qt.io/online/qtsdkrepository/windows_arm64/extensions/qtwebengine/$versionId/msvc2022_arm64"
[xml]$metadata = (Invoke-WebRequest -UseBasicParsing "$repository/Updates.xml").Content
$package = @($metadata.Updates.PackageUpdate | Where-Object {
    $_.Name -eq "extensions.qtwebengine.$versionId.win64_msvc2022_arm64"
})
if ($package.Count -ne 1 -or -not $package[0].DownloadableArchives) {
    throw "No Qt WebEngine $Version ARM64 package found at $repository"
}

foreach ($archive in $package[0].DownloadableArchives.Split(',')) {
    $archive = $archive.Trim()
    $url = "$repository/$($package[0].Name)/$($package[0].Version)$archive"
    $destination = Join-Path $QtDirectory $archive
    $checksumText = (Invoke-WebRequest -UseBasicParsing "$url.sha1").Content
    if ($checksumText -is [byte[]]) {
        $checksumText = [Text.Encoding]::ASCII.GetString($checksumText)
    }
    $checksum = ($checksumText.Trim() -split '\s+')[0]
    if ($checksum -notmatch '^[0-9a-fA-F]{40}$') {
        throw "Invalid checksum for $url"
    }

    Invoke-WebRequest -UseBasicParsing $url -OutFile $destination
    $stream = [IO.File]::OpenRead($destination)
    $sha1 = [Security.Cryptography.SHA1]::Create()
    try { $hash = [BitConverter]::ToString($sha1.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $sha1.Dispose() }
    if ($hash -ne $checksum) {
        throw "Checksum mismatch for $destination"
    }
    & $SevenZip x $destination "-o$qtRoot" -y -bso0
    if ($LASTEXITCODE -ne 0) {
        throw "7-Zip failed to extract $destination (exit $LASTEXITCODE)"
    }
    Remove-Item -LiteralPath $destination
}

if (-not (Test-Path -LiteralPath $config)) {
    throw "Qt WebEngine installation did not produce $config"
}
