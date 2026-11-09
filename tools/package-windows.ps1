# Assembles the single-file Windows installer dist/chast-<version>-setup.exe
# from an already built viewer tree, host binaries and the Go setup stub.
#
# Inputs (build them first):
#   cmake --build build --config Release --target chast-host chast-service
#   cd app; flutter build windows --release
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/package-windows.ps1 -Version 0.1.0
#
# The optional usbmmidd_v2 driver package (fixed hash, Amyuni redistribution
# permission required) is bundled only when it is present:
#   powershell ... -VirtualDisplayPackageDir <path-to-usbmmidd_v2>
# or when the repository already has drivers\usbmmidd_v2. Without it the
# installer still builds, but --virtual-display installs are rejected.

param(
    [string]$Version = "0.1.0",
    [string]$Configuration = "Release",
    [string]$OutDir = "dist",
    [string]$VirtualDisplayPackageDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$viewerDir = Join-Path $root "app\build\windows\x64\runner\Release"
$hostDir = Join-Path $root "build\$Configuration"
$serverDir = Join-Path $root "server"
$stage = Join-Path $root "build\package\chast-$Version"
$zip = Join-Path $root "build\package\chast-$Version.zip"
$stub = Join-Path $root "build\package\chast-setup-$Version.exe"
$out = Join-Path $root "$OutDir\chast-$Version-setup.exe"

function Require-File([string]$Path, [string]$Hint) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "missing $Path; $Hint"
    }
}

Require-File (Join-Path $viewerDir "chast_viewer.exe") "build the viewer with: cd app; flutter build windows --release"
Require-File (Join-Path $viewerDir "chast-tunnel.exe") "the viewer build ships the bundled tunnel agent"
Require-File (Join-Path $hostDir "chast-host.exe") "build the host with: cmake --build build --config $Configuration --target chast-host"
Require-File (Join-Path $hostDir "chast-service.exe") "build the service with: cmake --build build --config $Configuration --target chast-service"
Require-File (Join-Path $viewerDir "data\app.so") "the viewer release tree is incomplete"

Write-Host "staging chast $Version"
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -Path (Join-Path $viewerDir "*") -Destination $stage -Recurse -Force
Copy-Item -LiteralPath (Join-Path $hostDir "chast-host.exe") -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $hostDir "chast-service.exe") -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $root "THIRD-PARTY-NOTICES.md") -Destination $stage -Force
Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -in ".lib", ".pdb", ".exp" } |
    Remove-Item -Force

# The viewer Release directory is a build tree: acceptance probes and other
# temporary tools may be dropped next to the app while testing. Only the four
# product executables belong in the installer.
$productExes = @("chast_viewer.exe", "chast-tunnel.exe", "chast-host.exe", "chast-service.exe")
Get-ChildItem -LiteralPath $stage -File -Filter *.exe |
    Where-Object { $_.Name -notin $productExes } |
    Remove-Item -Force

$driverSource = $VirtualDisplayPackageDir
if (-not $driverSource) {
    $defaultDriverSource = Join-Path $root "drivers\usbmmidd_v2"
    if (Test-Path -LiteralPath $defaultDriverSource -PathType Container) {
        $driverSource = $defaultDriverSource
    }
}
if ($driverSource) {
    $driverFiles = @(
        "deviceinstaller64.exe", "usbmmidd.inf", "usbmmidd.cat",
        "x64\usbmmidd.dll", "win32\usbmmidd.dll", "License.txt", "manifest.sha256"
    )
    foreach ($name in $driverFiles) {
        Require-File (Join-Path $driverSource $name) "the virtual display package is incomplete"
    }
    $driverStage = Join-Path $stage "drivers\usbmmidd_v2"
    New-Item -ItemType Directory -Path $driverStage -Force | Out-Null
    Copy-Item -Path (Join-Path $driverSource "*") -Destination $driverStage -Recurse -Force
    Write-Host "bundled the usbmmidd virtual display driver"
}
else {
    Write-Warning "no usbmmidd_v2 package found; --virtual-display installs will be rejected"
}

Write-Host "archiving $zip"
if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

Write-Host "building the setup stub"
Push-Location $serverDir
try {
    go build -trimpath -ldflags "-X main.version=$Version" -o $stub ./cmd/chast-setup
    if ($LASTEXITCODE -ne 0) {
        throw "go build ./cmd/chast-setup failed"
    }
    if (-not (Test-Path -LiteralPath (Split-Path -Parent $out))) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
    }
    go run ./cmd/chast-package --stub $stub --payload $zip --output $out
    if ($LASTEXITCODE -ne 0) {
        throw "chast-package failed"
    }
}
finally {
    Pop-Location
}

$info = Get-Item -LiteralPath $out
$hash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host ""
Write-Host "installer: $($info.FullName)"
Write-Host "size:      $($info.Length)"
Write-Host "sha256:    $hash"
Write-Host ""
Write-Host "To serve this build as an automatic update, copy the file into --software-dir and set"
Write-Host "  --software-version $Version --software-url <public base URL>/software/$($info.Name)"
