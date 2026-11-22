param(
    [string]$BinaryDirectory = (Join-Path $PSScriptRoot '..\build\root\Release'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\test-temp')
)

# Hardware regression: captures the local desktop into an isolated output
# directory, then decodes every access unit. No system configuration changes.
$ErrorActionPreference = 'Stop'
$rateEncoder = Join-Path $BinaryDirectory 'chast-encode.exe'
$rateDecoder = Join-Path $BinaryDirectory 'chast-decode.exe'
foreach ($rateTool in @($rateEncoder, $rateDecoder)) {
    if (-not (Test-Path -LiteralPath $rateTool -PathType Leaf)) {
        throw "Missing executable: $rateTool"
    }
}
$rateRun = Join-Path $OutputDirectory ('avc-rate-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $rateRun -Force | Out-Null
Write-Output "Evidence directory: $rateRun"
Get-FileHash -LiteralPath $rateEncoder, $rateDecoder -Algorithm SHA256 |
    Format-List | Out-String | Set-Content -LiteralPath (Join-Path $rateRun 'binaries.txt')
$rateCases = @(
    @{ Name = 'congestion'; InitialRate = 2859; InitialFps = 30; Rate = 698; Fps = 15 },
    @{ Name = 'trickle'; InitialRate = 698; InitialFps = 15; Rate = 698; Fps = 1 },
    @{ Name = 'recovery'; InitialRate = 698; InitialFps = 1; Rate = 2859; Fps = 30 }
)
foreach ($rateCase in $rateCases) {
    $rateStream = Join-Path $rateRun ($rateCase.Name + '.h264')
    $rateEncodeLog = Join-Path $rateRun ($rateCase.Name + '-encode.log')
    $rateDecodeLog = Join-Path $rateRun ($rateCase.Name + '-decode.log')
    $rateSwitch = '1000:{0}:{1}' -f $rateCase.Rate, $rateCase.Fps
    & $rateEncoder --encoder vpl --codec h264 --bitrate $rateCase.InitialRate `
        --framerate $rateCase.InitialFps --seconds 4 --bitrate-switch $rateSwitch `
        --out $rateStream *> $rateEncodeLog
    if ($LASTEXITCODE -ne 0) { throw "AVC encode failed: $rateEncodeLog" }
    $rateEncoded = Get-Content -Raw -LiteralPath $rateEncodeLog
    $rateFrameMatch = [regex]::Match($rateEncoded, 'encoded=(\d+) keyframes=\d+ errors=0')
    if ($rateEncoded -notmatch 'encoder: H\.264' -or
        $rateEncoded -notmatch ('rate switched to {0} kbps, {1} fps' -f $rateCase.Rate, $rateCase.Fps) -or
        -not $rateFrameMatch.Success) {
        throw "AVC target or encode count mismatch: $rateEncodeLog"
    }
    $rateExpectedFrames = [int]$rateFrameMatch.Groups[1].Value
    if ($rateExpectedFrames -lt 2) { throw "Insufficient encoded frames: $rateEncodeLog" }
    & $rateDecoder --in $rateStream --codec h264 --quiet *> $rateDecodeLog
    if ($LASTEXITCODE -ne 0) { throw "AVC decode failed: $rateDecodeLog" }
    $rateDecoded = Get-Content -Raw -LiteralPath $rateDecodeLog
    if ($rateDecoded -notmatch ('units={0} keyframes=\d+ decoded={0} errors=0' -f $rateExpectedFrames)) {
        throw "AVC decode did not consume every encoded frame: $rateDecodeLog"
    }
    Write-Output ('PASS {0}: {1} kbps/{2} FPS, {3} frames decoded without errors' -f
        $rateCase.Name, $rateCase.Rate, $rateCase.Fps, $rateExpectedFrames)
}
