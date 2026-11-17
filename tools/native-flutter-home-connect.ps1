param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$PeerId
)

$ErrorActionPreference = 'Stop'
$token = [Console]::In.ReadLine()
if ([string]::IsNullOrWhiteSpace($token)) {
    throw 'The test host token was not provided on standard input.'
}

Add-Type -AssemblyName System.Windows.Forms
$deadline = [DateTime]::UtcNow.AddSeconds(20)
do {
    $viewer = Get-Process -Id $ProcessId -ErrorAction Stop
    if ($viewer.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)
if ($viewer.MainWindowHandle -eq 0) {
    throw 'The Flutter home window did not appear.'
}

$shell = New-Object -ComObject WScript.Shell
Start-Sleep -Seconds 2
if (-not $shell.AppActivate($ProcessId)) {
    throw 'Could not activate the Flutter home window.'
}
Start-Sleep -Milliseconds 500
[System.Windows.Forms.SendKeys]::SendWait($PeerId)
[System.Windows.Forms.SendKeys]::SendWait('{TAB}')
[System.Windows.Forms.SendKeys]::SendWait($token)
[System.Windows.Forms.SendKeys]::SendWait('+{TAB}')
[System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
Write-Output "Submitted the Flutter viewer connection for $PeerId."
