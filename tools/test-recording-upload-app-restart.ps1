param(
    [string]$FlutterExecutable = 'D:\flutter\bin\flutter.bat',
    [string]$DartExecutable = 'D:\flutter\bin\cache\dart-sdk\bin\dart.exe',
    [int]$TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$appRoot = Join-Path $repoRoot 'app'
$appProbe = Join-Path $appRoot 'tool\recording_upload_app_probe.dart'
$fixture = Join-Path $appRoot 'tool\recording_upload_acceptance_fixture.dart'
foreach ($path in @($FlutterExecutable, $DartExecutable, $appProbe, $fixture)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required test input not found: $path"
    }
}

$tempRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\test-temp'))
[void][IO.Directory]::CreateDirectory($tempRoot)
$runId = [guid]::NewGuid().ToString('N')
$tempDir = [IO.Path]::GetFullPath((Join-Path $tempRoot "chast-upload-app-$runId"))
if (Test-Path -LiteralPath $tempDir) { throw 'The unique acceptance-test directory already exists.' }

[void][IO.Directory]::CreateDirectory($tempDir)
$appData = Join-Path $tempDir 'appdata'
$chastData = Join-Path $appData 'chast'
$recording = Join-Path $chastData 'restart-fixture.mp4'
$queue = Join-Path $chastData 'recording-uploads.json'
$portFile = Join-Path $tempDir 'server-port'
$patchStarted = Join-Path $tempDir 'patch-started'
$releasePatch = Join-Path $tempDir 'release-patch'
$patchReleased = Join-Path $tempDir 'patch-released'
$complete = Join-Path $tempDir 'upload-complete'
$ready = Join-Path $tempDir 'app-ready'
$trace = Join-Path $tempDir 'server-port.jsonl'
$serverProcess = $null
$firstApp = $null
$secondApp = $null
$previousAppData = $env:APPDATA
$flutterConfigAppData = Join-Path $tempDir 'flutter-config'
$probeBuildStarted = $false
$appStarted = $false
$testPassed = $false

function Quote-Argument([string]$value) {
    return '"' + ($value -replace '"', '\"') + '"'
}

function Start-Child([string]$file, [string[]]$arguments, [string]$workingDirectory, [hashtable]$environment) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $file
    $info.WorkingDirectory = $workingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Arguments = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    foreach ($key in $environment.Keys) { $info.Environment[$key] = $environment[$key] }
    return [Diagnostics.Process]::Start($info)
}

function Wait-ForFile([string]$path, [Diagnostics.Process]$process, [string]$stage) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        if ($process -and $process.HasExited) {
            $stderr = $process.StandardError.ReadToEnd()
            throw "$stage process exited with $($process.ExitCode): $stderr"
        }
        if ([DateTime]::UtcNow -ge $deadline) { throw "Timed out waiting for $stage." }
        Start-Sleep -Milliseconds 50
    }
}

function Hide-TestWindow([int]$processId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        [ChastUploadProbeWindow]::HideProcessWindows([uint32]$processId)
        Start-Sleep -Milliseconds 100
    }
}

function Stop-OwnedProcess(
    [Diagnostics.Process]$process,
    [switch]$Graceful,
    [string]$DiagnosticDirectory
) {
    if (-not $process) { return }
    if (-not $process.HasExited) {
        if ($Graceful) {
            [void]$process.CloseMainWindow()
            if (-not $process.WaitForExit(5000)) { $process.Kill() }
        } else {
            $process.Kill()
        }
        $process.WaitForExit()
    }
    if ($DiagnosticDirectory) {
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        if ($stdout) {
            [IO.File]::WriteAllText(
                (Join-Path $DiagnosticDirectory "app-$($process.Id).stdout.log"),
                $stdout,
                [System.Text.UTF8Encoding]::new($false)
            )
        }
        if ($stderr) {
            [IO.File]::WriteAllText(
                (Join-Path $DiagnosticDirectory "app-$($process.Id).stderr.log"),
                $stderr,
                [System.Text.UTF8Encoding]::new($false)
            )
        }
    }
    $process.Dispose()
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class ChastUploadProbeWindow {
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hwnd, int command);

    public static void HideProcessWindows(uint targetProcessId) {
        EnumWindows((hwnd, _) => {
            uint owner;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner == targetProcessId) ShowWindow(hwnd, 0);
            return true;
        }, IntPtr.Zero);
    }
}
'@

try {
    [void][IO.Directory]::CreateDirectory($flutterConfigAppData)

    $serverProcess = Start-Child $DartExecutable @(
        'run', $fixture, 'serve', $recording, $portFile, $patchStarted,
        $releasePatch, $patchReleased, $complete, $trace, $ready
    ) $appRoot @{}
    Wait-ForFile $portFile $serverProcess 'HTTP fixture'
    $port = [int](Get-Content -LiteralPath $portFile -Raw)
    $serverUrl = "http://127.0.0.1:$port"

    $env:APPDATA = $appData
    & $DartExecutable run $fixture seed $queue $recording $serverUrl
    if ($LASTEXITCODE -ne 0) { throw 'Could not seed the isolated recording queue.' }
    $config = [ordered]@{
        server = $serverUrl
        queue = $queue
    }
    [IO.File]::WriteAllText(
        (Join-Path $tempDir 'probe-config.json'),
        ($config | ConvertTo-Json -Compress),
        [System.Text.UTF8Encoding]::new($false)
    )

    $env:APPDATA = $flutterConfigAppData
    Push-Location $appRoot
    try {
        $probeBuildStarted = $true
        & $FlutterExecutable build windows --target=tool/recording_upload_app_probe.dart "--dart-define=CHAST_UPLOAD_ACCEPTANCE_CONFIG=$(Join-Path $tempDir 'probe-config.json')" --release --no-pub | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Flutter Windows acceptance-probe build failed.' }
    } finally {
        Pop-Location
    }
    $assembleProject = Join-Path $appRoot 'build\windows\x64\flutter\flutter_assemble.vcxproj'
    if (-not (Test-Path -LiteralPath $assembleProject -PathType Leaf)) {
        throw "Flutter build settings were not generated: $assembleProject"
    }
    $buildSettings = Get-Content -LiteralPath $assembleProject -Raw
    [IO.File]::WriteAllText(
        (Join-Path $tempDir 'probe-build-settings.txt'),
        $buildSettings,
        [System.Text.UTF8Encoding]::new($false)
    )
    if ($buildSettings -notmatch 'FLUTTER_TARGET=tool[\\/]recording_upload_app_probe\.dart') {
        throw 'Flutter did not compile the acceptance probe as its Dart target.'
    }
    $env:APPDATA = $previousAppData

    $releaseDir = Join-Path $appRoot 'build\windows\x64\runner\Release'
    $appExecutable = Join-Path $releaseDir 'chast_viewer.exe'
    if (-not (Test-Path -LiteralPath $appExecutable -PathType Leaf)) {
        throw "Acceptance app was not built: $appExecutable"
    }
    $appEnvironment = @{
        APPDATA = $appData
        CHAST_PROXY = ''
    }
    $appArguments = @()

    $appStarted = $true
    $firstApp = Start-Child $appExecutable $appArguments $releaseDir $appEnvironment
    Wait-ForFile $ready $firstApp 'Flutter first frame'
    Hide-TestWindow $firstApp.Id
    Wait-ForFile $patchStarted $firstApp 'first upload chunk'
    $queueState = Get-Content -LiteralPath $queue -Raw | ConvertFrom-Json
    if ($queueState.jobs.Count -ne 1 -or
        $queueState.jobs[0].upload_id -ne 'abcdef0123456789abcdef0123456789') {
        throw 'The upload session id was not durably saved before the first process was terminated.'
    }

    Stop-OwnedProcess $firstApp -DiagnosticDirectory $tempDir
    $firstApp = $null
    [IO.File]::WriteAllText($releasePatch, 'release')
    Wait-ForFile $patchReleased $serverProcess 'fixture release after process termination'

    Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
    $secondApp = Start-Child $appExecutable $appArguments $releaseDir $appEnvironment
    Wait-ForFile $ready $secondApp 'Flutter restart first frame'
    Hide-TestWindow $secondApp.Id
    Wait-ForFile $complete $secondApp 'resumed archive completion'

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $queueState = Get-Content -LiteralPath $queue -Raw | ConvertFrom-Json
        if ($queueState.jobs.Count -eq 0) { break }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'The resumed upload completed remotely but remained queued locally.' }
        Start-Sleep -Milliseconds 50
    } while ($true)

    $events = @(Get-Content -LiteralPath $trace | ForEach-Object { $_ | ConvertFrom-Json })
    if (@($events | Where-Object { $_.method -eq 'POST' -and $_.path -eq '/v1/account/recordings/uploads' }).Count -ne 1 -or
        @($events | Where-Object { $_.method -eq 'GET' -and $_.path -match [regex]::Escape('/' + 'abcdef0123456789abcdef0123456789') }).Count -lt 1 -or
        @($events | Where-Object { $_.method -eq 'POST' -and $_.path -match '/complete$' }).Count -ne 1 -or
        @($events | Where-Object { $_.completed_sha256 }).Count -ne 1) {
        throw 'The HTTP trace did not show one idempotent start, a resume status read and one completion.'
    }

    $testPassed = $true
    Write-Host 'PASS: Flutter Windows process persisted upload progress, was force-terminated, then resumed over HTTP and removed the completed job.'
} finally {
    Stop-OwnedProcess $firstApp -DiagnosticDirectory $tempDir
    Stop-OwnedProcess $secondApp -Graceful -DiagnosticDirectory $tempDir
    if ($serverProcess) {
        if (-not $serverProcess.HasExited) {
            [IO.File]::WriteAllText("$portFile.stop", 'stop')
            if (-not $serverProcess.WaitForExit(5000)) { $serverProcess.Kill() }
        }
        $serverProcess.Dispose()
    }
    if ($probeBuildStarted) {
        try {
            $env:APPDATA = $flutterConfigAppData
            Push-Location $appRoot
            & $FlutterExecutable build windows --release --no-pub | Out-Host
            $restoreExitCode = $LASTEXITCODE
            Pop-Location
            if ($restoreExitCode -ne 0) {
                Write-Warning 'Could not restore the normal Flutter product build after the acceptance probe.'
            }
        } catch {
            if ((Get-Location).Path -eq $appRoot) { Pop-Location }
            Write-Warning "Could not restore the normal Flutter product build: $_"
        }
    }
    $env:APPDATA = $previousAppData

    $resolvedTemp = [IO.Path]::GetFullPath($tempDir)
    $tempPrefix = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\test-temp')).TrimEnd('\') + '\'
    if (-not $resolvedTemp.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside the temporary directory: $resolvedTemp"
    }
    if (-not $testPassed -and $appStarted) {
        Write-Warning "Acceptance diagnostics retained at: $resolvedTemp"
    } elseif ($testPassed) {
        Write-Host "Acceptance evidence retained at: $resolvedTemp"
    } elseif (Test-Path -LiteralPath $resolvedTemp) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
