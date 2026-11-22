param(
    [string]$DartExecutable,
    [int]$LockMilliseconds = 3000
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$appRoot = Join-Path $repoRoot 'app'
$probe = Join-Path $appRoot 'tool\recording_upload_worker_probe.dart'

if (-not $DartExecutable) {
    $dartCommand = Get-Command dart.exe -ErrorAction SilentlyContinue
    if ($dartCommand) {
        $DartExecutable = $dartCommand.Source
    } else {
        throw 'Pass -DartExecutable or add dart.exe to PATH.'
    }
}
if (-not (Test-Path -LiteralPath $DartExecutable -PathType Leaf)) {
    throw "Dart executable not found: $DartExecutable"
}
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw "Worker probe not found: $probe"
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempDir = Join-Path $tempRoot ("chast-upload-worker-" + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($tempDir)
$queueFile = Join-Path $tempDir 'queue.json'
$readyFile = Join-Path $tempDir 'worker-ready'
$holder = $null

function Invoke-Probe([string[]]$Arguments) {
    Push-Location $appRoot
    try {
        & $DartExecutable run $probe @Arguments
        return $LASTEXITCODE
    } finally {
        Pop-Location
    }
}

function Start-Worker([int]$DurationMs) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $DartExecutable
    $startInfo.WorkingDirectory = $appRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $escaped = @('run', $probe, 'hold', $queueFile, $readyFile, "$DurationMs") |
        ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }
    $startInfo.Arguments = $escaped -join ' '
    return [Diagnostics.Process]::Start($startInfo)
}

try {
    if ((Invoke-Probe @('seed', $queueFile)) -ne 0 -or
        (Invoke-Probe @('verify', $queueFile)) -ne 0) {
        throw 'A recording job did not survive a separate Dart process.'
    }

    $holder = Start-Worker $LockMilliseconds
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (-not (Test-Path -LiteralPath $readyFile)) {
        if ($holder.HasExited) {
            throw "Lock holder exited early with code $($holder.ExitCode)."
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw 'Timed out waiting for the child process to acquire the worker lock.'
        }
        Start-Sleep -Milliseconds 50
    }
    if ((Invoke-Probe @('probe', $queueFile)) -ne 10) {
        throw 'A second process acquired the worker lock while it was held.'
    }
    $holder.WaitForExit()
    if ($holder.ExitCode -ne 0) {
        throw "Lock holder failed with exit code $($holder.ExitCode)."
    }
    if ((Invoke-Probe @('verify', $queueFile, 'progress')) -ne 0) {
        throw 'A completed child process did not preserve its upload progress.'
    }
    if ((Invoke-Probe @('probe', $queueFile)) -ne 0) {
        throw 'The worker lock was not released after graceful process exit.'
    }

    Remove-Item -LiteralPath $readyFile -Force
    $holder = Start-Worker 60000
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (-not (Test-Path -LiteralPath $readyFile)) {
        if ($holder.HasExited) {
            throw "Crash-test lock holder exited early with code $($holder.ExitCode)."
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw 'Timed out waiting for the crash-test process to acquire the lock.'
        }
        Start-Sleep -Milliseconds 50
    }
    $holder.Kill()
    $holder.WaitForExit()
    $holder.Dispose()
    $holder = $null
    if ((Invoke-Probe @('verify', $queueFile, 'progress')) -ne 0) {
        throw 'A terminated child process did not preserve its upload progress.'
    }
    if ((Invoke-Probe @('probe', $queueFile)) -ne 0) {
        throw 'The worker lock remained held after terminating its owner process.'
    }

    Write-Host 'PASS: queue survived process recreation; worker lock excluded a second process and recovered after owner termination.'
} finally {
    if ($holder -and -not $holder.HasExited) {
        $holder.Kill()
        $holder.WaitForExit()
    }
    if ($holder) { $holder.Dispose() }
    $resolvedTemp = [IO.Path]::GetFullPath($tempDir)
    if (-not $resolvedTemp.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside the temporary directory: $resolvedTemp"
    }
    Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
}
