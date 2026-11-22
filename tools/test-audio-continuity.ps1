param(
    [ValidateRange(1, 100)][int]$Rounds = 5,
    [string]$Binary = (Join-Path $PSScriptRoot '..\build\root\Release\chast-quic.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\test-temp'),
    [ValidateRange(1024, 65534)][int]$Port = 45671
)

# Synthetic audio, muted WASAPI output and local QUIC/netem. Run after builds
# finish, without other media probes, so their load does not bias continuity.
$ErrorActionPreference = 'Stop'
$continuityBinary = (Resolve-Path -LiteralPath $Binary).Path
$continuityRun = Join-Path $OutputDirectory ('audio-continuity-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $continuityRun -Force | Out-Null
$continuityRun = (Resolve-Path -LiteralPath $continuityRun).Path
$continuityHash = (Get-FileHash -LiteralPath $continuityBinary -Algorithm SHA256).Hash
$continuityResults = [Collections.Generic.List[object]]::new()
$continuityFailures = 0
Write-Output "Evidence directory: $continuityRun"
Write-Output "Binary SHA256: $continuityHash"
Push-Location $continuityRun
try {
    for ($round = 1; $round -le $Rounds; $round++) {
        foreach ($mode in @('clean', 'weak')) {
            $continuityArgs = @('verify', '--port', "$Port", '--audio', '--play',
                '--mute-playback', '--quiet', '--seconds', '25', '--adaptive-jitter',
                '--audio-arrival-phase-correction', '--audio-render-buffer-ms', '10',
                '--audio-late-tolerance-ms', '20', '--audio-preserve-reanchor-queue',
                '--audio-playout-trace', '--audio-max-starved-ms', '2000',
                '--audio-max-device-gap-ms', '1000', '--audio-max-dropped-blocks', '200',
                '--audio-max-abs-av-skew-ms')
            if ($mode -eq 'weak') { $continuityArgs += @('120', '--netem') }
            else { $continuityArgs += '40' }
            $continuityLog = Join-Path $continuityRun "$mode-$round.log"
            $continuityStarted = Get-Date
            Write-Output "Starting $mode round $round at $($continuityStarted.ToString('HH:mm:ss'))"
            $ErrorActionPreference = 'Continue'
            & $continuityBinary @continuityArgs *> $continuityLog
            $continuityExit = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            $continuityText = Get-Content -Raw -LiteralPath $continuityLog
            $continuityAudio = [regex]::Match($continuityText,
                'audio-continuity: starved=(\d+) ms device-max-gap=(\d+) ms dropped=(\d+) av-skew=([+\-\d.]+) ms available=(\d+) budget=(PASS|FAIL)')
            $continuityAge = [regex]::Match($continuityText, 'max frame age ([\d.]+) ms')
            $continuitySameBinary = (Get-FileHash -LiteralPath $continuityBinary -Algorithm SHA256).Hash -eq $continuityHash
            $continuityPassed = $continuityExit -eq 0 -and $continuityAudio.Success -and
                $continuityAudio.Groups[6].Value -eq 'PASS' -and $continuitySameBinary
            if (-not $continuityPassed) { $continuityFailures++ }
            $continuityResults.Add([ordered]@{
                mode = $mode; round = $round; exitCode = $continuityExit; passed = $continuityPassed
                startedAt = $continuityStarted.ToString('o'); log = $continuityLog
                starvedMs = if ($continuityAudio.Success) { [int]$continuityAudio.Groups[1].Value } else { $null }
                deviceMaxGapMs = if ($continuityAudio.Success) { [int]$continuityAudio.Groups[2].Value } else { $null }
                droppedBlocks = if ($continuityAudio.Success) { [int]$continuityAudio.Groups[3].Value } else { $null }
                avSkewMs = if ($continuityAudio.Success) { [double]::Parse($continuityAudio.Groups[4].Value, [Globalization.CultureInfo]::InvariantCulture) } else { $null }
                videoMaxAgeMs = if ($continuityAge.Success) { [double]::Parse($continuityAge.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) } else { $null }
                sameBinary = $continuitySameBinary
            })
            [ordered]@{
                binary = $continuityBinary; sha256 = $continuityHash; plannedRounds = $Rounds
                failedRuns = $continuityFailures; results = $continuityResults.ToArray()
            } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $continuityRun 'results.json')
            Write-Output "$mode round $round exit=$continuityExit passed=$continuityPassed"
            Select-String -LiteralPath $continuityLog -Pattern '^audio-continuity:', '^QUIC .* (PASSED|FAILED)' |
                ForEach-Object { $_.Line }
            if (-not $continuitySameBinary) { throw 'Binary changed during the batch; results cannot be combined.' }
        }
    }
} finally {
    Pop-Location
}
Write-Output "Completed: $continuityFailures failing runs out of $($Rounds * 2)"
if ($continuityFailures) { exit 1 }
