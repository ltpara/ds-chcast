# Fetches and verifies the optional Amyuni usbmmidd_v2 virtual display driver
# package. The output directory is what chast-host.exe --vdisplay install
# expects and what tools/package-windows.ps1 bundles.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/fetch-usbmmidd.ps1
#   powershell ... -File tools/fetch-usbmmidd.ps1 -SourceZip C:\downloads\usbmmidd_v2.zip
#   powershell ... -File tools/fetch-usbmmidd.ps1 -Force
#
# The package is never committed: the output lives under build/ (gitignored).
#
# Fail-closed: the ZIP and every required payload must match reviewed hashes.

param(
    [string]$SourceZip = "",
    [string]$Destination = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$downloadUrl = "https://www.amyuni.com/downloads/usbmmidd_v2.zip"

# Official 2.0.0.1 package reviewed on 2026-11-05. This pin authorizes
# fetching for local use; it does not authorize bundling a release.
$pinnedZipSha256 = "629b51e9944762bae73948171c65d09a79595cf4c771a82ebc003fbba5b24f51"

$requiredFiles = @(
    "deviceinstaller64.exe", "usbmmidd.inf", "usbmmidd.cat",
    "x64\usbmmidd.dll", "win32\usbmmidd.dll", "License.txt"
)
$expectedSigner = "Microsoft Windows Hardware Compatibility Publisher"
$pinnedFiles = @{
    "deviceinstaller64.exe" = "e9baa02cdae921acf0aae4d8e8c29a4cdf4057ab61f9c60862b7cc439e2753f7"
    "usbmmidd.inf" = "a8750ca15a86742f3012886c9932bb974158cd2d9779cf891c730d976a47726a"
    "usbmmidd.cat" = "470149c4cf9970ba59070aa7c9409c9f63a15727de99bab53e7e51f55310779f"
    "x64\usbmmidd.dll" = "dc135d675127113915a7e5aa9fe57c84edad6be41d0890b265ef124ab26ea9e3"
    "win32\usbmmidd.dll" = "07ceebaecf203fef39117576d8b49d803c0fb1f6c7aa5175a6ebab110d54fed2"
    "License.txt" = "44c73fc4110979899dbb5054662de581f7d4350939f19bb2d50574f613314ba2"
}

if (-not $Destination) {
    $Destination = Join-Path $root "build\deps\usbmmidd_v2"
}
$Destination = [System.IO.Path]::GetFullPath($Destination)
if ($Destination.Length -gt 3) {
    $Destination = $Destination.TrimEnd('\', '/')
}

function Fail([int]$Code, [string]$Message) {
    Write-Host ""
    [Console]::Error.WriteLine("error: $Message")
    exit $Code
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-Package([string]$Root, [switch]$VerifyManifest) {
    foreach ($name in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $name) -PathType Leaf)) {
            return $false
        }
        if ((Get-Sha256 (Join-Path $Root $name)) -ne $pinnedFiles[$name]) {
            return $false
        }
    }
    if (-not $VerifyManifest) {
        return $true
    }
    $manifest = Join-Path $Root "manifest.sha256"
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        return $false
    }
    foreach ($line in Get-Content -LiteralPath $manifest) {
        $line = $line.Trim()
        if (-not $line -or $line.StartsWith("#")) {
            continue
        }
        $parts = $line -split '\s+', 2
        if ($parts.Count -ne 2) {
            return $false
        }
        $relative = $parts[1].Trim().TrimStart("*") -replace '/', '\'
        $file = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            return $false
        }
        if ((Get-Sha256 $file) -ne $parts[0].ToLowerInvariant()) {
            return $false
        }
    }
    return $true
}

if ($pinnedZipSha256 -notmatch '^[0-9a-fA-F]{64}$') {
    if ($SourceZip -and (Test-Path -LiteralPath $SourceZip -PathType Leaf)) {
        $resolved = (Resolve-Path -LiteralPath $SourceZip).Path
        Write-Host "local zip: $resolved"
        Write-Host "sha256:    $(Get-Sha256 $resolved)"
    }
    Fail 2 ("the pinned SHA-256 is not configured in tools/fetch-usbmmidd.ps1; " +
        "review the package, set `$pinnedZipSha256, and rerun")
}
$pinnedZipSha256 = $pinnedZipSha256.ToLowerInvariant()

$zipPath = $SourceZip
$downloaded = $false
if (-not $zipPath) {
    $downloadDir = Join-Path $root "build\deps"
    New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
    $zipPath = Join-Path $downloadDir "usbmmidd_v2.zip"
    Write-Host "downloading $downloadUrl"
    try {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    }
    catch {
    }
    Invoke-WebRequest -UseBasicParsing -Uri $downloadUrl -OutFile $zipPath
    $downloaded = $true
}
else {
    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        Fail 3 "missing $zipPath"
    }
    $zipPath = (Resolve-Path -LiteralPath $zipPath).Path
}

$zipHash = Get-Sha256 $zipPath
if ($zipHash -ne $pinnedZipSha256) {
    if ($downloaded) {
        Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue
    }
    Fail 3 "zip SHA-256 mismatch: expected $pinnedZipSha256, got $zipHash"
}
Write-Host "verified zip sha256: $zipHash"

if (Test-Path -LiteralPath $Destination -PathType Container) {
    if (-not $Force) {
        if (Test-Package $Destination -VerifyManifest) {
            Write-Host "already present and verified: $Destination"
            exit 0
        }
        Fail 4 "destination exists but does not verify; rerun with -Force to replace it"
    }
    Remove-Item -LiteralPath $Destination -Recurse -Force
}

$extract = Join-Path $root "build\deps\usbmmidd_v2.extract"
if (Test-Path -LiteralPath $extract) {
    Remove-Item -LiteralPath $extract -Recurse -Force
}
New-Item -ItemType Directory -Path $extract -Force | Out-Null
Expand-Archive -LiteralPath $zipPath -DestinationPath $extract -Force

$candidates = @($extract)
$candidates += Get-ChildItem -LiteralPath $extract -Directory |
    ForEach-Object { $_.FullName }
$packageCandidates = @($candidates | Where-Object { Test-Package $_ })
if ($packageCandidates.Count -ne 1) {
    Remove-Item -LiteralPath $extract -Recurse -Force
    Fail 4 "the zip does not contain exactly one complete usbmmidd_v2 package"
}
$packageRoot = $packageCandidates[0]

foreach ($name in @("usbmmidd.cat", "x64\usbmmidd.dll", "win32\usbmmidd.dll")) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $packageRoot $name)
    if ($signature.Status -ne "Valid" -or
        $signature.SignerCertificate.Subject -notlike "*$expectedSigner*") {
        Fail 4 "the driver signature is not trusted: $name ($($signature.Status))"
    }
    Write-Host "$name signer: $($signature.SignerCertificate.Subject)"
}
Write-Host "The vendor helper is unsigned; it is authorized by the reviewed package hash."

New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Copy-Item -Path (Join-Path $packageRoot "*") -Destination $Destination -Recurse -Force

$manifestLines = @()
Get-ChildItem -LiteralPath $Destination -Recurse -File |
    Where-Object { $_.Name -notin @("manifest.sha256", "SOURCE.txt") } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($Destination.Length).TrimStart('\', '/') -replace '\\', '/'
        $manifestLines += "$(Get-Sha256 $_.FullName)  $relative"
    }
if (-not $manifestLines) {
    Remove-Item -LiteralPath $extract -Recurse -Force
    Fail 4 "no files were extracted"
}
Set-Content -LiteralPath (Join-Path $Destination "manifest.sha256") -Value $manifestLines -Encoding ASCII

$version = ""
$driverVer = Select-String -LiteralPath (Join-Path $Destination "usbmmidd.inf") `
    -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)\s*$' | Select-Object -First 1
if ($driverVer) {
    $version = $driverVer.Matches[0].Groups[1].Value
}
$sourceLines = @(
    "URL: $downloadUrl",
    "SHA256: $zipHash"
)
if ($downloaded) {
    $sourceLines += "DOWNLOADED: $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))"
}
else {
    $sourceLines += "DOWNLOADED: no (verified local zip)"
}
$sourceLines += "VERSION: $version"
$sourceLines += "UNMODIFIED: yes"
Set-Content -LiteralPath (Join-Path $Destination "SOURCE.txt") -Value $sourceLines -Encoding ASCII

Remove-Item -LiteralPath $extract -Recurse -Force

if (-not (Test-Package $Destination -VerifyManifest)) {
    Fail 4 "the staged package failed its final verification"
}

Write-Host ""
Write-Host "package: $Destination"
Write-Host "version: $version"
Write-Host "files:   $($manifestLines.Count)"
Write-Host ""
Write-Host "Bundle it with:"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools/package-windows.ps1 -VirtualDisplayPackageDir `"$Destination`""
