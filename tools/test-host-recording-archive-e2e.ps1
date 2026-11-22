param(
    [string]$BuildDirectory = "build/root",
    [string]$CMakeExecutable = "cmake",
    [string]$GoExecutable = "go"
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $repoRoot $BuildDirectory
}
$producer = Join-Path $buildRoot 'Release/chast-recording-upload-queue-test.exe'
$parentHarness = Join-Path $buildRoot 'Release/chast-host-tunnel-test.exe'
$hostBinary = Join-Path $buildRoot 'Release/chast-host.exe'
$viewerProbe = Join-Path $buildRoot 'Release/chast-live-viewer-probe.exe'
if (-not (Test-Path -LiteralPath $parentHarness -PathType Leaf)) {
    throw "C++ TunnelProcess parent harness was not created: $parentHarness"
}
& $CMakeExecutable --build $buildRoot --config Release --target chast-host chast-live-viewer-probe chast-recording-upload-queue-test chast-host-tunnel-test
if ($LASTEXITCODE -ne 0) {
    throw "C++ host recording acceptance target build failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $producer -PathType Leaf)) {
    throw "C++ recording queue producer was not created: $producer"
}

$scratch = Join-Path $repoRoot ("build/test-temp/host-recording-archive-e2e-" +
    [Guid]::NewGuid().ToString('N'))
$queue = Join-Path $scratch 'queue'
$processScratch = Join-Path $scratch 'process'
$processQueue = Join-Path $processScratch 'queue'
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
New-Item -ItemType Directory -Path $processScratch -Force | Out-Null
& $producer --emit-fixture $queue
if ($LASTEXITCODE -ne 0) {
    throw "C++ agent recording fixture failed with exit code $LASTEXITCODE"
}
& $producer --emit-fixture $processQueue 4194404
if ($LASTEXITCODE -ne 0) {
    throw "C++ process recording fixture failed with exit code $LASTEXITCODE"
}

$previousQueue = $env:CHAST_CPP_RECORDING_QUEUE
$previousProcessQueue = $env:CHAST_CPP_RECORDING_PROCESS_QUEUE
$previousHarness = $env:CHAST_CPP_TUNNEL_PROCESS_TEST
$previousNativeBin = $env:CHAST_NATIVE_BIN_DIR
$previousNativeArchive = $env:CHAST_NATIVE_ARCHIVE_ACCEPTANCE
$env:CHAST_CPP_RECORDING_QUEUE = $queue
$env:CHAST_CPP_RECORDING_PROCESS_QUEUE = $processQueue
$env:CHAST_CPP_TUNNEL_PROCESS_TEST = $parentHarness
Push-Location (Join-Path $repoRoot 'server')
try {
    & $GoExecutable test ./internal/agent -run '^TestHostRecordingUploadConsumesCppManifest$' -count=1 -v
    if ($LASTEXITCODE -ne 0) {
        throw "Go recording upload worker acceptance failed with exit code $LASTEXITCODE"
    }
    & $GoExecutable test ./cmd/chast-tunnel -run '^TestHostRecordingUploadProcessResumesAfterKill$' -count=1 -v
    if ($LASTEXITCODE -ne 0) {
        throw "chast-tunnel host process acceptance failed with exit code $LASTEXITCODE"
    }
    & $GoExecutable test ./cmd/chast-tunnel -run '^TestHostRecordingUploadThroughCppParentResumesAfterKill$' -count=1 -v
    if ($LASTEXITCODE -ne 0) {
        throw "C++ TunnelProcess host recording acceptance failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $hostBinary -PathType Leaf) -or
        -not (Test-Path -LiteralPath $viewerProbe -PathType Leaf)) {
        throw 'The actual host or desktop viewer probe was not built.'
    }
    $env:CHAST_NATIVE_BIN_DIR = Join-Path $buildRoot 'Release'
    $env:CHAST_NATIVE_ARCHIVE_ACCEPTANCE = '1'
    & $GoExecutable test ./internal/api -run '^TestWindowsNativeHostRecordingArchiveAcceptance$' -count=1 -v
    if ($LASTEXITCODE -ne 0) {
        throw "Actual chast-host recording archive acceptance failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
    if ($null -eq $previousQueue) {
        Remove-Item Env:CHAST_CPP_RECORDING_QUEUE -ErrorAction SilentlyContinue
    } else {
        $env:CHAST_CPP_RECORDING_QUEUE = $previousQueue
    }
    if ($null -eq $previousProcessQueue) {
        Remove-Item Env:CHAST_CPP_RECORDING_PROCESS_QUEUE -ErrorAction SilentlyContinue
    } else {
        $env:CHAST_CPP_RECORDING_PROCESS_QUEUE = $previousProcessQueue
    }
    if ($null -eq $previousHarness) {
        Remove-Item Env:CHAST_CPP_TUNNEL_PROCESS_TEST -ErrorAction SilentlyContinue
    } else {
        $env:CHAST_CPP_TUNNEL_PROCESS_TEST = $previousHarness
    }
    if ($null -eq $previousNativeBin) {
        Remove-Item Env:CHAST_NATIVE_BIN_DIR -ErrorAction SilentlyContinue
    } else {
        $env:CHAST_NATIVE_BIN_DIR = $previousNativeBin
    }
    if ($null -eq $previousNativeArchive) {
        Remove-Item Env:CHAST_NATIVE_ARCHIVE_ACCEPTANCE -ErrorAction SilentlyContinue
    } else {
        $env:CHAST_NATIVE_ARCHIVE_ACCEPTANCE = $previousNativeArchive
    }
}

Write-Host "C++ queue and tunnel recovery plus real chast-host synthetic-video recording upload passed isolated loopback acceptance."
Write-Host "Acceptance files: $scratch"
