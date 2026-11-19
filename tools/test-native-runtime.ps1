# Opt-in, loopback-only Windows tray/tunnel acceptance. Requires an existing
# CMake build tree and Go. Does not install a service or alter host profiles.
[CmdletBinding()]
param([string]$BuildDirectory = 'build', [switch]$Browser, [switch]$Flutter, [switch]$DualFlutter, [switch]$MultiWindow, [switch]$FileTransfer, [switch]$ViewerStrategy, [switch]$InputInjection)

$ErrorActionPreference = 'Stop'
if ($Flutter -and -not $Browser -and -not $MultiWindow -and -not $FileTransfer -and -not $ViewerStrategy -and -not $InputInjection) {
    throw '-Flutter requires a browser, multi-window, file-transfer, viewer-strategy, or input-injection acceptance mode.'
}
if ($DualFlutter -and -not ($Flutter -and $Browser)) {
    throw '-DualFlutter requires both -Flutter and -Browser.'
}
if ($MultiWindow -and -not $Flutter) {
    throw '-MultiWindow requires -Flutter.'
}
if ($MultiWindow -and ($Browser -or $DualFlutter -or $FileTransfer -or $ViewerStrategy -or $InputInjection)) {
    throw '-MultiWindow runs as a dedicated Flutter-only acceptance; do not combine it with browser, dual-viewer, file-transfer, viewer-strategy, or input-injection modes.'
}
if ($FileTransfer -and -not $Flutter) {
    throw '-FileTransfer requires -Flutter.'
}
if ($ViewerStrategy -and -not $Flutter) {
    throw '-ViewerStrategy requires -Flutter.'
}
if ($ViewerStrategy -and ($Browser -or $DualFlutter -or $FileTransfer)) {
    throw '-ViewerStrategy runs as a dedicated Flutter-only acceptance; do not combine it with browser, dual-viewer, or file-transfer modes.'
}
if ($InputInjection -and -not $Flutter) {
    throw '-InputInjection requires -Flutter.'
}
if ($InputInjection -and ($Browser -or $DualFlutter -or $FileTransfer -or $ViewerStrategy)) {
    throw '-InputInjection runs as a dedicated Flutter-only acceptance; do not combine it with browser, dual-viewer, file-transfer, or viewer-strategy modes.'
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
$oldDualFlutter = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_DUAL_FLUTTER', 'Process')
$oldMultiWindow = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW', 'Process')
$oldFileTransfer = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_FILE_TRANSFER', 'Process')
$oldViewerStrategy = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_VIEWER_STRATEGY', 'Process')
$oldInputInjection = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_INPUT_INJECTION', 'Process')
Push-Location $repoRoot
try {
    & cmake --build $buildRoot --config Release --target chast-host chast-live-viewer-probe -j 4 *> (Join-Path $runRoot 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "Native build failed; see $runRoot\build.log" }
    if ($Flutter) {
        Push-Location (Join-Path $repoRoot 'app')
        try {
            $previousErrorActionPreference = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Continue'
                & flutter build windows --release *> (Join-Path $runRoot 'flutter-build.log')
                $flutterBuildExitCode = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $previousErrorActionPreference
            }
            if ($flutterBuildExitCode -ne 0) { throw "Flutter Release build failed; see $runRoot\flutter-build.log" }
        } finally {
            Pop-Location
        }
        $env:CHAST_NATIVE_FLUTTER_BIN = Join-Path $repoRoot 'app\build\windows\x64\runner\Release\chast_viewer.exe'
        $env:CHAST_NATIVE_FLUTTER_UI = '1'
        if ($DualFlutter) {
            $env:CHAST_NATIVE_DUAL_FLUTTER = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_DUAL_FLUTTER -ErrorAction SilentlyContinue
        }
        if ($MultiWindow) {
            $env:CHAST_NATIVE_MULTIWINDOW = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_MULTIWINDOW -ErrorAction SilentlyContinue
        }
        if ($FileTransfer) {
            $env:CHAST_NATIVE_FILE_TRANSFER = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_FILE_TRANSFER -ErrorAction SilentlyContinue
        }
        if ($ViewerStrategy) {
            $env:CHAST_NATIVE_VIEWER_STRATEGY = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_VIEWER_STRATEGY -ErrorAction SilentlyContinue
        }
        if ($InputInjection) {
            $env:CHAST_NATIVE_INPUT_INJECTION = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_INPUT_INJECTION -ErrorAction SilentlyContinue
        }
    } else {
        Remove-Item Env:CHAST_NATIVE_FLUTTER_UI -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_FLUTTER_BIN -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_DUAL_FLUTTER -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_MULTIWINDOW -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_FILE_TRANSFER -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_VIEWER_STRATEGY -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_INPUT_INJECTION -ErrorAction SilentlyContinue
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
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_DUAL_FLUTTER', $oldDualFlutter, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW', $oldMultiWindow, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_FILE_TRANSFER', $oldFileTransfer, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_VIEWER_STRATEGY', $oldViewerStrategy, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_INPUT_INJECTION', $oldInputInjection, 'Process')
    Pop-Location
}
