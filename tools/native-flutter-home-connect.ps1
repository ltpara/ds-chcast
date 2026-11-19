param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$PeerId,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [int]$ExpectedSessionCount = 1,
    [switch]$UseExistingCredentials
)

$ErrorActionPreference = 'Stop'
$token = [Console]::In.ReadLine()
if ([string]::IsNullOrWhiteSpace($token)) {
    throw 'The test host token was not provided on standard input.'
}
if ($ExpectedSessionCount -lt 1 -or $ExpectedSessionCount -gt 16) {
    throw 'ExpectedSessionCount must be between 1 and 16.'
}

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class FlutterHomeInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    public static long[] FindVisibleProcessWindows(int ownerProcessId) {
        var windows = new List<long>();
        EnumWindows((window, _) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == (uint)ownerProcessId && IsWindowVisible(window)) {
                windows.Add(window.ToInt64());
            }
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }
}
'@

function Capture-Window([IntPtr]$Window, [string]$Path) {
    $rect = New-Object FlutterHomeInput+Rect
    if (-not [FlutterHomeInput]::GetWindowRect($Window, [ref]$rect)) {
        throw 'Could not read the Flutter window bounds.'
    }
    $bitmap = New-Object Drawing.Bitmap(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [FlutterHomeInput]::PrintWindow($Window, $dc, 2)) {
            throw "Could not capture the Flutter window: $Path"
        }
    } finally {
        $graphics.ReleaseHdc($dc)
    }
    try {
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Send-Click([IntPtr]$Window, [int]$X, [int]$Y) {
    $point = [IntPtr]([int64]$X -bor ([int64]$Y -shl 16))
    [void][FlutterHomeInput]::SendMessageW($Window, 0x0200, [IntPtr]::Zero, $point)
    [void][FlutterHomeInput]::SendMessageW($Window, 0x0201, [IntPtr]1, $point)
    [void][FlutterHomeInput]::SendMessageW($Window, 0x0202, [IntPtr]::Zero, $point)
}

function Send-Text([IntPtr]$Window, [string]$Text) {
    foreach ($character in $Text.ToCharArray()) {
        if ([int]$character -gt 127) {
            throw 'The native Flutter acceptance input only supports ASCII credentials.'
        }
        [void][FlutterHomeInput]::SendMessageW($Window, 0x0102, [IntPtr][int]$character, [IntPtr]::Zero)
    }
}

$deadline = [DateTime]::UtcNow.AddSeconds(20)
do {
    $viewer = Get-Process -Id $ProcessId -ErrorAction Stop
    if ($viewer.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)
if ($viewer.MainWindowHandle -eq 0) {
    throw 'The Flutter home window did not appear.'
}
$homeHandlePath = Join-Path $ArtifactDirectory "flutter-home-window-$ProcessId.txt"
if ($UseExistingCredentials) {
    if (-not (Test-Path -LiteralPath $homeHandlePath)) {
        throw 'The saved Flutter home window handle was not found.'
    }
    $homeWindow = [IntPtr]([int64](Get-Content -LiteralPath $homeHandlePath -Raw))
} else {
    $homeWindow = [IntPtr]$viewer.MainWindowHandle
}

Start-Sleep -Milliseconds 500
$root = [Windows.Automation.AutomationElement]::FromHandle($homeWindow)
$condition = New-Object Windows.Automation.PropertyCondition(
    [Windows.Automation.AutomationElement]::ClassNameProperty, 'FLUTTERVIEW')
$view = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
if ($null -eq $view -or $view.Current.NativeWindowHandle -eq 0) {
    throw 'The Flutter render window was not exposed for native input.'
}
$viewHandle = [IntPtr]$view.Current.NativeWindowHandle
$client = New-Object FlutterHomeInput+Rect
if (-not [FlutterHomeInput]::GetClientRect($viewHandle, [ref]$client)) {
    throw 'Could not read the Flutter render client bounds.'
}
if (($client.Right - $client.Left) -lt 1100 -or ($client.Bottom - $client.Top) -lt 650) {
    throw "Flutter home window is smaller than the acceptance layout: $($client.Right - $client.Left)x$($client.Bottom - $client.Top)."
}

New-Item -ItemType Directory -Path $ArtifactDirectory -Force | Out-Null
if (-not $UseExistingCredentials) {
    Set-Content -LiteralPath $homeHandlePath -Value $homeWindow.ToInt64() -NoNewline -Encoding ASCII
    Capture-Window $homeWindow (Join-Path $ArtifactDirectory 'flutter-home-before-input.png')
}

# Target the Flutter HWND directly. SendKeys can silently type into another
# foreground/topmost application and falsely report a submitted connection.
if (-not $UseExistingCredentials) {
    Send-Click $viewHandle 664 167
    Send-Text $viewHandle $PeerId
    Send-Click $viewHandle 664 229
    Send-Text $viewHandle $token
    Capture-Window $homeWindow (Join-Path $ArtifactDirectory 'flutter-home-filled.png')
}
function Get-SessionWindows {
    @([FlutterHomeInput]::FindVisibleProcessWindows($ProcessId) |
        Where-Object { [IntPtr]([int64]$_) -ne $homeWindow })
}

$initialWindows = @(Get-SessionWindows)
if ($initialWindows.Count -ne ($ExpectedSessionCount - 1)) {
    throw "Expected $($ExpectedSessionCount - 1) session window(s) before submission; found $($initialWindows.Count)."
}
Send-Click $viewHandle 616 309

$sessionWindows = @(Get-SessionWindows)
$retryAt = [DateTime]::UtcNow.AddSeconds(4)
$deadline = [DateTime]::UtcNow.AddSeconds(14)
$retried = $false
while ($sessionWindows.Count -lt $ExpectedSessionCount -and [DateTime]::UtcNow -lt $deadline) {
    if (-not $retried -and [DateTime]::UtcNow -ge $retryAt) {
        if (-not $UseExistingCredentials) {
            Capture-Window $homeWindow (Join-Path $ArtifactDirectory 'flutter-home-after-submit.png')
        }
        Send-Click $viewHandle 616 309
        $retried = $true
    }
    Start-Sleep -Milliseconds 100
    $sessionWindows = @(Get-SessionWindows)
}
if ($sessionWindows.Count -ne $ExpectedSessionCount) {
    if (-not $UseExistingCredentials) {
        Capture-Window $homeWindow (Join-Path $ArtifactDirectory 'flutter-home-after-submit.png')
    }
    throw "The Flutter connect action opened $($sessionWindows.Count) session window(s); expected $ExpectedSessionCount."
}
Write-Output "Submitted Flutter connection for $PeerId through HWND $viewHandle; $ExpectedSessionCount session window(s) are open."
