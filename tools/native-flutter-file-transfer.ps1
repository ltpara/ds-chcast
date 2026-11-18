param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('Inspect', 'Upload', 'Download')][string]$Phase,
    [string]$UploadPath = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class NativeFlutterFileUi {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);
    private delegate bool EnumChildWindowsCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr dialog, int itemId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetWindowTextW", ExactSpelling = true)]
    public static extern int GetWindowTextW(IntPtr window, System.Text.StringBuilder text, int maxCount);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumChildWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW", ExactSpelling = true)]
    private static extern int GetClassNameW(IntPtr window, System.Text.StringBuilder name, int maxCount);
    [DllImport("user32.dll")]
    private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    public static IntPtr FindChild(IntPtr parent, string className, int controlId) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (window, _) => {
            var currentClass = new System.Text.StringBuilder(128);
            GetClassNameW(window, currentClass, currentClass.Capacity);
            if (currentClass.ToString() == className && GetDlgCtrlID(window) == controlId) {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
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
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr window);
    public static string[] DescribeChildren(IntPtr parent) {
        var controls = new List<string>();
        EnumChildWindows(parent, (window, _) => {
            var className = new System.Text.StringBuilder(128);
            var text = new System.Text.StringBuilder(512);
            GetClassNameW(window, className, className.Capacity);
            GetWindowTextW(window, text, text.Capacity);
            Rect rect;
            GetWindowRect(window, out rect);
            var name = className.ToString();
            if (name == "Edit" || name.Contains("Combo") || name == "Button") {
                controls.Add(String.Format("hwnd={0} class={1} id={2} visible={3} enabled={4} rect={5},{6},{7},{8} text={9}",
                    window.ToInt64(), name, GetDlgCtrlID(window), IsWindowVisible(window), IsWindowEnabled(window),
                    rect.Left, rect.Top, rect.Right, rect.Bottom, text.ToString()));
            }
            return true;
        }, IntPtr.Zero);
        return controls.ToArray();
    }
}
'@

function Capture-Window([IntPtr]$Window, [string]$Path) {
    $rect = New-Object NativeFlutterFileUi+Rect
    if (-not [NativeFlutterFileUi]::GetWindowRect($Window, [ref]$rect)) {
        throw 'Could not read the Flutter session window bounds.'
    }
    $bitmap = New-Object Drawing.Bitmap(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [NativeFlutterFileUi]::PrintWindow($Window, $dc, 2)) {
            throw "Could not capture the Flutter session window: $Path"
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
    [void][NativeFlutterFileUi]::SendMessageW($Window, 0x0200, [IntPtr]::Zero, $point)
    [void][NativeFlutterFileUi]::SendMessageW($Window, 0x0201, [IntPtr]1, $point)
    [void][NativeFlutterFileUi]::SendMessageW($Window, 0x0202, [IntPtr]::Zero, $point)
}

function Open-FileBrowser([IntPtr]$Window, [int]$Width, [string]$CapturePath) {
    Send-Click $Window ([int]($Width / 2)) 20
    Start-Sleep -Milliseconds 600
    Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-file-browser-after-first-toolbar-click.png')
    Send-Click $Window ([int]($Width / 2) - 20) 30
    Start-Sleep -Milliseconds 800
    Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-file-browser-after-second-toolbar-click.png')
    Capture-Window $sessionHandle $CapturePath
}

function Select-UploadFile([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The isolated upload fixture does not exist: $Path"
    }
    $dialogHandle = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 3 -and $dialogHandle -eq [IntPtr]::Zero; $attempt++) {
        Send-Click $viewHandle ($width - 113) 84
        $deadline = [DateTime]::UtcNow.AddSeconds(4)
        while ([DateTime]::UtcNow -lt $deadline) {
            $candidates = @([NativeFlutterFileUi]::FindVisibleProcessWindows($ProcessId) |
                Where-Object {
                    $candidate = [IntPtr]([int64]$_)
                    $candidate -ne $homeHandle -and $candidate -ne $sessionHandle -and
                        $candidate -ne $viewHandle
                })
            if ($candidates.Count -gt 0) {
                $dialogHandle = [IntPtr]([int64]$candidates[0])
                break
            }
            Start-Sleep -Milliseconds 100
        }
        if ($dialogHandle -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 300 }
    }
    if ($dialogHandle -eq [IntPtr]::Zero) {
        Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-file-picker-not-open.png')
        $visible = @([NativeFlutterFileUi]::FindVisibleProcessWindows($ProcessId) | ForEach-Object {
            $text = New-Object System.Text.StringBuilder 256
            [void][NativeFlutterFileUi]::GetWindowTextW([IntPtr]([int64]$_), $text, $text.Capacity)
            "$($_):$($text.ToString())"
        })
        Write-Output "visible process windows: $($visible -join ' | ')"
        throw 'The Windows upload file picker did not appear.'
    }

    $dialog = [Windows.Automation.AutomationElement]::FromHandle($dialogHandle)
    if ($dialog.Current.ProcessId -ne $ProcessId -or $dialog.Current.ClassName -ne '#32770') {
        throw 'The upload picker does not belong to the expected Flutter process or dialog class.'
    }

    $pickerPath = (Resolve-Path -LiteralPath $Path).Path
    $directory = [IO.Path]::GetDirectoryName($pickerPath)
    $addressBar = [NativeFlutterFileUi]::FindChild($dialogHandle, 'ToolbarWindow32', 1001)
    if ($addressBar -eq [IntPtr]::Zero) {
        throw 'The upload picker address bar was not found; refusing to select a same-named file from an unknown directory.'
    }
    $addressText = New-Object System.Text.StringBuilder 1024
    [void][NativeFlutterFileUi]::GetWindowTextW($addressBar, $addressText, $addressText.Capacity)
    if ($addressText.ToString().IndexOf($directory, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "The upload picker is not showing the fixture directory. Expected '$directory'; address was '$($addressText.ToString())'."
    }

    $listCondition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ClassNameProperty, 'UIItemsView')
    $list = $dialog.FindFirst([Windows.Automation.TreeScope]::Descendants, $listCondition)
    if ($null -eq $list -or $list.Current.ProcessId -ne $ProcessId) {
        throw 'The upload picker Shell item list was not found in the expected Flutter process.'
    }
    $containerPattern = $null
    if (-not $list.TryGetCurrentPattern(
        [Windows.Automation.ItemContainerPattern]::Pattern, [ref]$containerPattern)) {
        throw 'The upload picker Shell item list does not expose ItemContainerPattern.'
    }

    $fileName = [IO.Path]::GetFileName($pickerPath)
    $fileElement = $containerPattern.FindItemByProperty(
        $null, [Windows.Automation.AutomationElement]::NameProperty, $fileName)
    if ($null -eq $fileElement) {
        $dialog.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition) |
            ForEach-Object {
                "class=$($_.Current.ClassName) pid=$($_.Current.ProcessId) name=$($_.Current.Name) offscreen=$($_.Current.IsOffscreen)"
            } | Set-Content -LiteralPath (Join-Path $ArtifactDirectory 'flutter-upload-picker-uia.txt') -Encoding UTF8
        throw "The upload fixture '$fileName' was not found in the Shell item list; see the UIA diagnostic artifact."
    }
    if ($fileElement.Current.ProcessId -ne $ProcessId -or $fileElement.Current.Name -ne $fileName) {
        throw "The upload picker returned an unexpected item: pid=$($fileElement.Current.ProcessId), name='$($fileElement.Current.Name)'."
    }
    $virtualizedPattern = $null
    if ($fileElement.TryGetCurrentPattern(
        [Windows.Automation.VirtualizedItemPattern]::Pattern, [ref]$virtualizedPattern)) {
        $virtualizedPattern.Realize()
    }
    $scrollPattern = $null
    if ($fileElement.TryGetCurrentPattern(
        [Windows.Automation.ScrollItemPattern]::Pattern, [ref]$scrollPattern)) {
        $scrollPattern.ScrollIntoView()
    }
    $selectionPattern = $null
    if (-not $fileElement.TryGetCurrentPattern(
        [Windows.Automation.SelectionItemPattern]::Pattern, [ref]$selectionPattern)) {
        throw "The upload fixture '$fileName' does not expose SelectionItemPattern."
    }
    $selectionPattern.Select()
    Write-Output "upload picker controls: $([NativeFlutterFileUi]::DescribeChildren($dialogHandle) -join ' | ')"

    $okButton = [NativeFlutterFileUi]::GetDlgItem($dialogHandle, 1)
    if ($okButton -eq [IntPtr]::Zero) {
        throw 'The upload picker has no Open button.'
    }
    Start-Sleep -Milliseconds 150
    Capture-Window $dialogHandle (Join-Path $ArtifactDirectory 'flutter-upload-picker-selected.png')
    [void][NativeFlutterFileUi]::SendMessageW($okButton, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and [NativeFlutterFileUi]::IsWindow($dialogHandle)) {
        Start-Sleep -Milliseconds 100
    }
    if ([NativeFlutterFileUi]::IsWindow($dialogHandle)) {
        $invokePattern = $null
        if ($fileElement.TryGetCurrentPattern(
            [Windows.Automation.InvokePattern]::Pattern, [ref]$invokePattern)) {
            $invokePattern.Invoke()
            $deadline = [DateTime]::UtcNow.AddSeconds(5)
            while ([DateTime]::UtcNow -lt $deadline -and [NativeFlutterFileUi]::IsWindow($dialogHandle)) {
                Start-Sleep -Milliseconds 100
            }
        }
    }
    if ([NativeFlutterFileUi]::IsWindow($dialogHandle)) {
        Capture-Window $dialogHandle (Join-Path $ArtifactDirectory 'flutter-upload-picker-still-open.png')
        throw "The Windows upload file picker did not close after selecting '$pickerPath'."
    }
}

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
$sessionHandles = @([NativeFlutterFileUi]::FindVisibleProcessWindows($ProcessId) |
    Where-Object { [IntPtr]([int64]$_) -ne $homeHandle })
if ($sessionHandles.Count -ne 1) {
    throw "Expected one visible Flutter session window; found $($sessionHandles.Count)."
}
$sessionHandle = [IntPtr]([int64]$sessionHandles[0])
$root = [Windows.Automation.AutomationElement]::FromHandle($sessionHandle)
$condition = New-Object Windows.Automation.PropertyCondition(
    [Windows.Automation.AutomationElement]::ClassNameProperty, 'FLUTTERVIEW')
$view = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
if ($null -eq $view -or $view.Current.NativeWindowHandle -eq 0) {
    throw 'The Flutter session render window was not found.'
}
$viewHandle = [IntPtr]$view.Current.NativeWindowHandle
$client = New-Object NativeFlutterFileUi+Rect
if (-not [NativeFlutterFileUi]::GetClientRect($viewHandle, [ref]$client)) {
    throw 'Could not read the Flutter session render bounds.'
}
$width = $client.Right - $client.Left
$height = $client.Bottom - $client.Top
if ($width -lt 800 -or $height -lt 500) {
    throw "Flutter session is too small for file transfer acceptance: ${width}x${height}."
}

New-Item -ItemType Directory -Path $ArtifactDirectory -Force | Out-Null
Write-Output "session=$sessionHandle render=$viewHandle size=${width}x${height}"
if ($Phase -eq 'Inspect') {
    Send-Click $viewHandle ([int]($width / 2)) 20
    Start-Sleep -Milliseconds 700
    Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-session-toolbar-before.png')
    Send-Click $viewHandle ([int]($width / 2) - 20) 30
    Start-Sleep -Milliseconds 700
    Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-session-toolbar-after.png')
    Write-Output "Captured toolbar before/after candidate click in $ArtifactDirectory."
    exit 0
}

if ($Phase -eq 'Upload') {
    Open-FileBrowser $viewHandle $width (Join-Path $ArtifactDirectory 'flutter-file-browser-before-upload.png')
    Select-UploadFile $UploadPath
    Start-Sleep -Milliseconds 500
    Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-file-browser-after-upload-queued.png')
    Write-Output "Selected upload fixture through the Flutter file browser: $UploadPath"
    exit 0
}

Send-Click $viewHandle ($width - 53) 84
Start-Sleep -Milliseconds 900
Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-file-browser-download-list.png')
Send-Click $viewHandle ($width - 58) 216
Start-Sleep -Milliseconds 300
Capture-Window $sessionHandle (Join-Path $ArtifactDirectory 'flutter-file-browser-download-started.png')
Write-Output 'Requested a download from the visible fixture file row.'
