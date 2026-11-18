param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [Parameter(Mandatory = $true)][string]$StopFile,
    [int]$IntervalMilliseconds = 250,
    [int]$MaximumSamples = 360
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class FlutterFrameProbe {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);

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

$homeHandlePath = Join-Path $ArtifactDirectory "flutter-home-window-$ProcessId.txt"
$deadline = [DateTime]::UtcNow.AddSeconds(20)
while (-not (Test-Path -LiteralPath $homeHandlePath)) {
    if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
        throw "Flutter process $ProcessId exited before publishing its home window handle."
    }
    if ([DateTime]::UtcNow -ge $deadline) {
        throw 'The Flutter home window handle was not published.'
    }
    Start-Sleep -Milliseconds 50
}
$homeHandle = [IntPtr]([int64](Get-Content -LiteralPath $homeHandlePath -Raw))

$deadline = [DateTime]::UtcNow.AddSeconds(20)
do {
    $sessionHandles = @([FlutterFrameProbe]::FindVisibleProcessWindows($ProcessId) |
        Where-Object { [IntPtr]([int64]$_) -ne $homeHandle })
    if ($sessionHandles.Count -gt 0) { break }
    if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
        throw "Flutter process $ProcessId exited before opening its session window."
    }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)

if ($sessionHandles.Count -ne 1) {
    throw "Expected exactly one visible Flutter session window for PID $ProcessId; found $($sessionHandles.Count)."
}
$sessionHandle = [IntPtr]([int64]$sessionHandles[0])
$rect = New-Object FlutterFrameProbe+Rect
if (-not [FlutterFrameProbe]::GetWindowRect($sessionHandle, [ref]$rect)) {
    throw 'Could not read the Flutter session window bounds.'
}
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -lt 600 -or $height -lt 400) {
    throw "Flutter session window is too small to sample: ${width}x${height}."
}

$index = 0
while ($index -lt $MaximumSamples -and -not (Test-Path -LiteralPath $StopFile)) {
    if (-not [FlutterFrameProbe]::IsWindow($sessionHandle)) {
        throw 'The non-target Flutter session window closed during sampling.'
    }
    $bitmap = New-Object Drawing.Bitmap($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [FlutterFrameProbe]::PrintWindow($sessionHandle, $dc, 2)) {
            throw 'PrintWindow could not capture the non-target Flutter session.'
        }
    } finally {
        $graphics.ReleaseHdc($dc)
    }
    try {
        $path = Join-Path $ArtifactDirectory ('flutter-secondary-frame-{0:D4}.png' -f $index)
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    $index++
    Start-Sleep -Milliseconds $IntervalMilliseconds
}

if ($index -lt 6) {
    throw "Only $index Flutter session samples were captured."
}
Write-Output "Captured $index stats-enabled Flutter session windows from PID $ProcessId (${width}x${height})."
