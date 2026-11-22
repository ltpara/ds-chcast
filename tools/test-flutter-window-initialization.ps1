[CmdletBinding()]
param([string]$BuildDirectory = 'app/build/windows/x64')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $repo $BuildDirectory
}
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$release = Join-Path $build 'runner/Release'
if (-not (Test-Path -LiteralPath (Join-Path $release 'data/app.so'))) {
    throw 'Build Flutter Windows Release before running this regression.'
}
$evidence = Join-Path $repo ('build/test-temp/flutter-window-init-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $evidence | Out-Null
& cmake --build $build --config Release --target chast-flutter-view-initialization-test *> (Join-Path $evidence 'build.log')
if ($LASTEXITCODE -ne 0) { throw "Build failed; see $evidence" }
$test = Join-Path $release 'chast-flutter-view-initialization-test.exe'
$profile = Join-Path $evidence 'profile'
New-Item -ItemType Directory -Path $profile | Out-Null
$oldAppData = $env:APPDATA
$oldProfile = $env:USERPROFILE
try {
    $env:APPDATA = Join-Path $profile 'AppData/Roaming'
    $env:USERPROFILE = $profile
    & $test (Join-Path $release 'data') *> (Join-Path $evidence 'test.log')
    $testExit = $LASTEXITCODE
} finally {
    $env:APPDATA = $oldAppData
    $env:USERPROFILE = $oldProfile
}
Get-FileHash -LiteralPath $test, (Join-Path $release 'flutter_windows.dll') |
    Select-Object Path, Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'hashes.json')
Get-Content -LiteralPath (Join-Path $evidence 'test.log')
Write-Output "Evidence: $evidence"
exit $testExit
