# Opt-in, loopback-only Windows tray/tunnel acceptance. Requires an existing
# CMake build tree and Go. Does not install a service or alter host profiles.
[CmdletBinding()]
param([string]$BuildDirectory = 'build', [switch]$Browser, [switch]$Flutter, [switch]$DualFlutter, [switch]$MultiWindow, [switch]$MultiWindowInput, [switch]$MultiWindowAudio, [switch]$MultiWindowDisplay, [switch]$FileTransfer, [switch]$ViewerStrategy, [switch]$InputInjection, [switch]$KeyboardShortcuts)

$ErrorActionPreference = 'Stop'
if ($Flutter -and -not $Browser -and -not $MultiWindow -and -not $MultiWindowAudio -and -not $MultiWindowDisplay -and -not $FileTransfer -and -not $ViewerStrategy -and -not $InputInjection -and -not $KeyboardShortcuts) {
    throw '-Flutter requires a browser, multi-window, multi-window-audio/display, file-transfer, viewer-strategy, input-injection, or keyboard-shortcut acceptance mode.'
}
if ($DualFlutter -and -not ($Flutter -and $Browser)) {
    throw '-DualFlutter requires both -Flutter and -Browser.'
}
if ($MultiWindow -and -not $Flutter) {
    throw '-MultiWindow requires -Flutter.'
}
if ($MultiWindowInput -and -not $MultiWindow) {
    throw '-MultiWindowInput requires -MultiWindow.'
}
if ($MultiWindowAudio -and -not $Flutter) {
    throw '-MultiWindowAudio requires -Flutter.'
}
if ($MultiWindowDisplay -and -not $Flutter) {
    throw '-MultiWindowDisplay requires -Flutter.'
}
if ($MultiWindow -and ($Browser -or $DualFlutter -or $FileTransfer -or $ViewerStrategy -or $InputInjection -or $KeyboardShortcuts -or $MultiWindowAudio -or $MultiWindowDisplay)) {
    throw '-MultiWindow runs as a dedicated Flutter-only acceptance; do not combine it with browser, dual-viewer, file-transfer, viewer-strategy, or input-injection modes.'
}
if ($MultiWindowAudio -and ($Browser -or $DualFlutter -or $MultiWindow -or $MultiWindowInput -or $MultiWindowDisplay -or $FileTransfer -or $ViewerStrategy -or $InputInjection -or $KeyboardShortcuts)) {
    throw '-MultiWindowAudio runs as a dedicated Flutter-only acceptance.'
}
if ($MultiWindowDisplay -and ($Browser -or $DualFlutter -or $MultiWindow -or $MultiWindowInput -or $MultiWindowAudio -or $FileTransfer -or $ViewerStrategy -or $InputInjection -or $KeyboardShortcuts)) {
    throw '-MultiWindowDisplay runs as a dedicated Flutter-only acceptance.'
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
if ($KeyboardShortcuts -and -not $Flutter) {
    throw '-KeyboardShortcuts requires -Flutter.'
}
if ($KeyboardShortcuts -and ($Browser -or $DualFlutter -or $MultiWindow -or $MultiWindowInput -or $MultiWindowAudio -or $MultiWindowDisplay -or $FileTransfer -or $ViewerStrategy -or $InputInjection)) {
    throw '-KeyboardShortcuts runs as a dedicated Flutter-only acceptance.'
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
$oldMultiWindowInput = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW_INPUT', 'Process')
$oldMultiWindowAudio = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW_AUDIO', 'Process')
$oldMultiWindowDisplay = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW_DISPLAY', 'Process')
$oldFileTransfer = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_FILE_TRANSFER', 'Process')
$oldViewerStrategy = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_VIEWER_STRATEGY', 'Process')
$oldInputInjection = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_INPUT_INJECTION', 'Process')
$oldKeyboardShortcuts = [Environment]::GetEnvironmentVariable('CHAST_NATIVE_KEYBOARD_SHORTCUTS', 'Process')
Push-Location $repoRoot
try {
    & cmake --build $buildRoot --config Release --target chast-host chast-live-viewer-probe -j 4 *> (Join-Path $runRoot 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "Native build failed; see $runRoot\build.log" }
    if ($MultiWindowAudio -or $MultiWindowDisplay) {
        $fixtureName = if ($MultiWindowAudio) { 'audio' } else { 'display' }
        & cmake --build $buildRoot --config Release --target chast-viewer-probe -j 4 *> (Join-Path $runRoot "$fixtureName-fixture-build.log")
        if ($LASTEXITCODE -ne 0) { throw "Synthetic $fixtureName fixture build failed; see $runRoot\$fixtureName-fixture-build.log" }
    }
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
        if ($MultiWindowInput) {
            $env:CHAST_NATIVE_MULTIWINDOW_INPUT = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_MULTIWINDOW_INPUT -ErrorAction SilentlyContinue
        }
        if ($MultiWindowAudio) {
            $env:CHAST_NATIVE_MULTIWINDOW_AUDIO = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_MULTIWINDOW_AUDIO -ErrorAction SilentlyContinue
        }
        if ($MultiWindowDisplay) {
            $env:CHAST_NATIVE_MULTIWINDOW_DISPLAY = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_MULTIWINDOW_DISPLAY -ErrorAction SilentlyContinue
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
        if ($KeyboardShortcuts) {
            $env:CHAST_NATIVE_KEYBOARD_SHORTCUTS = '1'
        } else {
            Remove-Item Env:CHAST_NATIVE_KEYBOARD_SHORTCUTS -ErrorAction SilentlyContinue
        }
    } else {
        Remove-Item Env:CHAST_NATIVE_FLUTTER_UI -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_FLUTTER_BIN -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_DUAL_FLUTTER -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_MULTIWINDOW -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_MULTIWINDOW_INPUT -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_MULTIWINDOW_AUDIO -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_MULTIWINDOW_DISPLAY -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_FILE_TRANSFER -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_VIEWER_STRATEGY -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_INPUT_INJECTION -ErrorAction SilentlyContinue
        Remove-Item Env:CHAST_NATIVE_KEYBOARD_SHORTCUTS -ErrorAction SilentlyContinue
    }
    $env:CHAST_NATIVE_BIN_DIR = Join-Path $buildRoot 'Release'
    $env:CHAST_NATIVE_ARTIFACT_DIR = $runRoot
    if ($Browser) {
        $env:CHAST_NATIVE_BROWSER_UI = '1'
    } else {
        Remove-Item Env:CHAST_NATIVE_BROWSER_UI -ErrorAction SilentlyContinue
    }
    Set-Location (Join-Path $repoRoot 'server')
    $testName = '^TestWindowsNativeRuntimeAcceptance$'
    if ($MultiWindowAudio) { $testName = '^TestWindowsNativeFlutterTabAudioAcceptance$' }
    if ($MultiWindowDisplay) { $testName = '^TestWindowsNativeFlutterDisplayWindowLifecycleAcceptance$' }
    & go test ./internal/api -run $testName -v -count=1 -timeout 6m *> (Join-Path $runRoot 'acceptance.log')
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
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW_INPUT', $oldMultiWindowInput, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW_AUDIO', $oldMultiWindowAudio, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_MULTIWINDOW_DISPLAY', $oldMultiWindowDisplay, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_FILE_TRANSFER', $oldFileTransfer, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_VIEWER_STRATEGY', $oldViewerStrategy, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_INPUT_INJECTION', $oldInputInjection, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_NATIVE_KEYBOARD_SHORTCUTS', $oldKeyboardShortcuts, 'Process')
    Pop-Location
}
