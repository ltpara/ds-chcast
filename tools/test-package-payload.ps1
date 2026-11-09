$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package-payload.ps1')
$fixture = Join-Path (Split-Path -Parent $PSScriptRoot) "build\package-test-$([Guid]::NewGuid().ToString('N'))"
$source = Join-Path $fixture 'source'
$stage = Join-Path $fixture 'stage'
$sentinel = 'FAKE-PACKAGING-SECRET-DO-NOT-SHIP'
foreach ($name in $ViewerPayloadFiles) {
    $path = Join-Path $source $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value 'product fixture'
}
foreach ($name in @('chast-portable\host.ini', 'probe.exe', 'notes.txt', 'unknown.dll',
                    'data\private.ini', 'data\flutter_assets\private.json')) {
    $path = Join-Path $source $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value $sentinel
}
Copy-PayloadFiles $source $stage $ViewerPayloadFiles
$files = @(Get-ChildItem -LiteralPath $stage -File -Recurse)
if ($files.Count -ne $ViewerPayloadFiles.Count) { throw 'unexpected payload files' }
foreach ($file in $files) {
    if ((Get-Content -LiteralPath $file.FullName -Raw).Contains($sentinel)) {
        throw 'runtime data escaped into payload'
    }
}
$archive = Join-Path $fixture 'payload.zip'
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archiveFile = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $entries = @($archiveFile.Entries | Where-Object { $_.Name -ne '' })
    if ($entries.Count -ne $ViewerPayloadFiles.Count) { throw 'unexpected archive contents' }
    foreach ($entry in $entries) {
        $reader = [IO.StreamReader]::new($entry.Open())
        try {
            if ($reader.ReadToEnd().Contains($sentinel)) { throw 'secret entered the archive' }
        }
        finally { $reader.Dispose() }
    }
}
finally { $archiveFile.Dispose() }
$driver = Join-Path $fixture 'driver'
$manifest = foreach ($name in $DriverPayloadFiles) {
    $path = Join-Path $driver $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value 'driver fixture'
    "$((Get-FileHash -LiteralPath $path).Hash)  $name"
}
Set-Content -LiteralPath (Join-Path $driver 'manifest.sha256') -Value $manifest
Set-Content -LiteralPath (Join-Path $driver 'host.ini') -Value $sentinel
$driverStage = Join-Path $fixture 'driver-stage'
Copy-DriverPayload $driver $driverStage
if (Test-Path -LiteralPath (Join-Path $driverStage 'host.ini')) { throw 'driver runtime data escaped' }
# The redistributed manifest remains complete and verifies independently.
Copy-DriverPayload $driverStage (Join-Path $fixture 'verified-driver')
Set-Content -LiteralPath (Join-Path $driver 'usbmmidd.inf') -Value 'tampered'
$rejected = $false
try { Copy-DriverPayload $driver (Join-Path $fixture 'bad-driver') }
catch { $rejected = $true }
if (-not $rejected) { throw 'tampered driver accepted' }
$rejected = $false
try { Copy-PayloadFiles $source (Join-Path $fixture 'missing-stage') @('missing.dll') }
catch { $rejected = $true }
if (-not $rejected) { throw 'missing release file accepted' }
Write-Host 'package payload checks passed (product allowlist, secret exclusion, driver hashes, missing inputs)'
