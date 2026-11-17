# Opt-in, loopback-only Windows tray/tunnel acceptance. Requires an existing
# CMake build tree and Go. Does not install a service or alter host profiles.
[CmdletBinding()]
param([string]$BuildDirectory = 'build', [switch]$Browser, [switch]$Flutter)

$ErrorActionPreference = 'Stop'
if ($Flutter -and -not $Browser) {
    throw '-Flutter requires -Browser so the real Flutter session is disconnected through the management page.'
}
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot $BuildDirectory
}
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt'))) {
    throw 'Configure the CMake build tree before running this acceptance.'
}
$runName = 'native-runtime-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runRoot = Join-Path $repoRoot "build\$runName"
New-Item -ItemType Directory -Path $runRoot | Out-Null
$oldBin = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_BIN_DIR', 'Process')
$oldArtifacts = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_ARTIFACT_DIR', 'Process')
$oldBrowserUI = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_BROWSER_UI', 'Process')
$oldFlutterUI = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_FLUTTER_UI', 'Process')
$oldFlutterBin = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_FLUTTER_BIN', 'Process')
Push-Location $repoRoot
try {
    & cmake --build $buildRoot --config Release --target chast-host chast-live-viewer-probe -j 4 *> (Join-Path $runRoot 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "Native build failed; see $runRoot\build.log" }
    if ($Flutter) {
        Push-Location (Join-Path $repoRoot 'app')
        try {
            & flutter build windows --release *> (Join-Path $runRoot 'flutter-build.log')
            if ($LASTEXITCODE -ne 0) { throw "Flutter Release build failed; see $runRoot\flutter-build.log" }
        } finally {
            Pop-Location
        }
        $env:CHAST_NATIVE_FLUTTER_BIN = Join-Path $repoRoot 'app\build\windows\x64\runner\Release\chast_viewer.exe'
        $env:CHAST_NATIVE_FLUTTER_UI = '1'
    } else {
        Remove-Item Env:CHAST_NATIVE_FLUTTER_UI -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_FLUTTER_BIN -ErrorAction SilentlyContinue
    }
    $env:CHAST_NATIVE_BIN_DIR = Join-Path $buildRoot 'Release'
    $env:CHAST_NATIVE_ARTIFACT_DIR = $runRoot
    if ($Browser) {
        $env:CHAST_NATIVE_BROWSER_UI = '1'
    } else {
        Remove-Item Env:CHAST_NATIVE_BROWSER_UI -ErrorAction SilentlyContinue
    }
    Set-Location (Join-Path $repoRoot 'server')
    & go test ./internal/api -run '^TestWindowsNativeRuntimeAcceptance$' -v -count=1 -timeout 6m *> (Join-Path $runRoot 'acceptance.log')
    $testExit = $LASTEXITCODE
    Get-Content -LiteralPath (Join-Path $runRoot 'acceptance.log')
    if ($testExit -ne 0) { throw "Native process acceptance failed; see $runRoot" }
    Write-Output "Native process acceptance passed. Artifacts: $runRoot"
} finally {
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_BIN_DIR', $oldBin, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_ARTIFACT_DIR', $oldArtifacts, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_BROWSER_UI', $oldBrowserUI, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_FLUTTER_UI', $oldFlutterUI, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_FLUTTER_BIN', $oldFlutterBin, 'Process')
    Pop-Location
}
