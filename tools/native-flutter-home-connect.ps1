param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$PeerId,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [int]$ExpectedSessionCount = 1,
    [switch]$UseExistingCredentials,
    [switch]$AllowEmptyToken,
    [switch]$CloseSelectedTab,
    [switch]$ExpectCloseFailure
)

$ErrorActionPreference = 'Stop'
$token = [Console]::In.ReadLine()
if ([string]::IsNullOrWhiteSpace($token) -and -not $AllowEmptyToken) {
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
using System.Text;
public static class FlutterHomeInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X, Y; }
    public struct SessionTabState {
        public IntPtr Host;
        public IntPtr Tabs;
        public int Count;
        public int Selected;
        public int SessionWindows;
        public int VisibleSessionWindows;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct TabItem {
        public uint Mask;
        public uint State;
        public uint StateMask;
        public IntPtr Text;
        public int TextCapacity;
        public int Image;
        public IntPtr Param;
    }
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);
    private delegate bool EnumChildWindowsCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")]
    public static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", EntryPoint = "GetDlgItem")]
    public static extern IntPtr GetDlgItem(IntPtr window, int controlId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr process, IntPtr address,
        byte[] buffer, UIntPtr size, out UIntPtr bytesWritten);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        byte[] buffer, UIntPtr size, out UIntPtr bytesRead);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address,
        UIntPtr size, uint allocationType, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        out Rect buffer, UIntPtr size, out UIntPtr bytesRead);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFreeEx(IntPtr process, IntPtr address,
        UIntPtr size, uint freeType);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumChildWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")]
    private static extern int GetClassName(IntPtr window, StringBuilder className, int maxCount);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetWindowTextW")]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int maxCount);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    public static string DescribeProcessWindows(int ownerProcessId) {
        var result = new StringBuilder();
        EnumWindows((window, _) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId != (uint)ownerProcessId) return true;
            var className = new StringBuilder(128);
            GetClassName(window, className, className.Capacity);
            var title = new StringBuilder(256);
            GetWindowText(window, title, title.Capacity);
            result.Append("top hwnd=").Append(window.ToInt64())
                .Append(" class=").Append(className)
                .Append(" visible=").Append(IsWindowVisible(window))
                .Append(" title=").AppendLine(title.ToString());
            EnumChildWindows(window, (child, __) => {
                uint childProcessId;
                GetWindowThreadProcessId(child, out childProcessId);
                if (childProcessId != (uint)ownerProcessId) return true;
                var childClass = new StringBuilder(128);
                GetClassName(child, childClass, childClass.Capacity);
                var childTitle = new StringBuilder(256);
                GetWindowText(child, childTitle, childTitle.Capacity);
                result.Append("  child hwnd=").Append(child.ToInt64())
                    .Append(" parent=").Append(GetParent(child).ToInt64())
                    .Append(" class=").Append(childClass)
                    .Append(" visible=").Append(IsWindowVisible(child));
                if (childClass.ToString() == "SysTabControl32") {
                    result.Append(" count=")
                        .Append(SendMessageW(child, 0x1304, IntPtr.Zero, IntPtr.Zero).ToInt64())
                        .Append(" selected=")
                        .Append(SendMessageW(child, 0x130B, IntPtr.Zero, IntPtr.Zero).ToInt64());
                }
                result.Append(" title=").AppendLine(childTitle.ToString());
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        if (result.Length == 0) return "<no top-level windows>";
        return result.ToString();
    }
    public static bool GetTabItemRect(IntPtr tabs, int index, out Rect rect) {
        rect = new Rect();
        uint processId;
        GetWindowThreadProcessId(tabs, out processId);
        var process = OpenProcess(0x0018, false, processId);
        if (process == IntPtr.Zero) return false;
        var size = new UIntPtr((uint)Marshal.SizeOf(typeof(Rect)));
        var remoteRect = VirtualAllocEx(process, IntPtr.Zero, size, 0x3000, 0x04);
        if (remoteRect == IntPtr.Zero) {
            CloseHandle(process);
            return false;
        }
        try {
            if (SendMessageW(tabs, 0x130A, (IntPtr)index, remoteRect) == IntPtr.Zero)
                return false;
            UIntPtr bytesRead;
            if (!ReadProcessMemory(process, remoteRect, out rect, size, out bytesRead) ||
                bytesRead.ToUInt64() != (ulong)Marshal.SizeOf(typeof(Rect)))
                return false;
            return rect.Right > rect.Left && rect.Bottom > rect.Top;
        } finally {
            VirtualFreeEx(process, remoteRect, UIntPtr.Zero, 0x8000);
            CloseHandle(process);
        }
    }
    public static string[] GetTabLabels(IntPtr tabs) {
        int count = (int)SendMessageW(tabs, 0x1304, IntPtr.Zero, IntPtr.Zero).ToInt64();
        if (count <= 0) return new string[0];
        uint processId;
        GetWindowThreadProcessId(tabs, out processId);
        // The tab control is in another process: allocate the TCITEMW descriptor
        // and its text buffer there, then read back only the public tab captions.
        var process = OpenProcess(0x0038, false, processId);
        if (process == IntPtr.Zero) return new string[0];
        int itemSize = Marshal.SizeOf(typeof(TabItem));
        const int textCapacity = 1200;
        int textBytes = textCapacity * 2;
        var remoteItem = VirtualAllocEx(process, IntPtr.Zero,
            new UIntPtr((uint)itemSize), 0x3000, 0x04);
        var remoteText = VirtualAllocEx(process, IntPtr.Zero,
            new UIntPtr((uint)textBytes), 0x3000, 0x04);
        if (remoteItem == IntPtr.Zero || remoteText == IntPtr.Zero) {
            if (remoteItem != IntPtr.Zero) VirtualFreeEx(process, remoteItem, UIntPtr.Zero, 0x8000);
            if (remoteText != IntPtr.Zero) VirtualFreeEx(process, remoteText, UIntPtr.Zero, 0x8000);
            CloseHandle(process);
            return new string[0];
        }
        var labels = new List<string>();
        IntPtr localItem = Marshal.AllocHGlobal(itemSize);
        try {
            for (int index = 0; index < count; ++index) {
                var item = new TabItem {
                    Mask = 0x0001, // TCIF_TEXT
                    Text = remoteText,
                    TextCapacity = textCapacity
                };
                Marshal.StructureToPtr(item, localItem, false);
                var descriptor = new byte[itemSize];
                Marshal.Copy(localItem, descriptor, 0, itemSize);
                UIntPtr written;
                if (!WriteProcessMemory(process, remoteItem, descriptor,
                    new UIntPtr((uint)itemSize), out written) ||
                    written.ToUInt64() != (ulong)itemSize)
                    return new string[0];
                if (SendMessageW(tabs, 0x133C, new IntPtr(index), remoteItem) == IntPtr.Zero)
                    return new string[0];
                var text = new byte[textBytes];
                UIntPtr read;
                if (!ReadProcessMemory(process, remoteText, text,
                    new UIntPtr((uint)textBytes), out read) || read.ToUInt64() < 2)
                    return new string[0];
                labels.Add(Encoding.Unicode.GetString(text).TrimEnd('\0'));
            }
            return labels.ToArray();
        } finally {
            Marshal.FreeHGlobal(localItem);
            VirtualFreeEx(process, remoteText, UIntPtr.Zero, 0x8000);
            VirtualFreeEx(process, remoteItem, UIntPtr.Zero, 0x8000);
            CloseHandle(process);
        }
    }
    public static string FindVisibleWindowText(int ownerProcessId, string expected,
        string expectedClassName) {
        string found = "";
        EnumWindows((window, _) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId != (uint)ownerProcessId || !IsWindowVisible(window)) return true;
            var className = new StringBuilder(128);
            GetClassName(window, className, className.Capacity);
            if (!String.Equals(className.ToString(), expectedClassName,
                StringComparison.Ordinal)) return true;
            var text = new StringBuilder(2048);
            GetWindowText(window, text, text.Capacity);
            if (String.Equals(text.ToString(), expected, StringComparison.Ordinal)) {
                found = text.ToString();
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindVisibleWindowHandle(int ownerProcessId, string expected,
        string expectedClassName) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, _) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId != (uint)ownerProcessId || !IsWindowVisible(window)) return true;
            var className = new StringBuilder(128);
            GetClassName(window, className, className.Capacity);
            if (!String.Equals(className.ToString(), expectedClassName,
                StringComparison.Ordinal)) return true;
            var text = new StringBuilder(2048);
            GetWindowText(window, text, text.Capacity);
            if (String.Equals(text.ToString(), expected, StringComparison.Ordinal)) {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static bool ClickTabClose(IntPtr tabs, int index) {
        Rect rect;
        if (!GetTabItemRect(tabs, index, out rect)) return false;
        int x = rect.Right - 15;
        int y = (rect.Top + rect.Bottom) / 2;
        long point = ((long)(ushort)y << 16) | (ushort)x;
        SendMessageW(tabs, 0x0201, new IntPtr(1), new IntPtr(point));
        SendMessageW(tabs, 0x0202, IntPtr.Zero, new IntPtr(point));
        return true;
    }
    public static SessionTabState FindSessionTabs(int ownerProcessId) {
        var state = new SessionTabState {
            Host = IntPtr.Zero,
            Tabs = IntPtr.Zero,
            Count = 0,
            Selected = -1,
            SessionWindows = 0,
            VisibleSessionWindows = 0
        };
        EnumWindows((window, _) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == (uint)ownerProcessId) {
                EnumChildWindows(window, (child, __) => {
                    var className = new StringBuilder(64);
                    GetClassName(child, className, className.Capacity);
                    if (className.ToString() == "SysTabControl32") {
                        var count = (int)SendMessageW(child, 0x1304,
                            IntPtr.Zero, IntPtr.Zero).ToInt64();
                        if (count > state.Count) {
                            state.Host = GetParent(child);
                            state.Tabs = child;
                            state.Count = count;
                            state.Selected = (int)SendMessageW(child, 0x130B,
                                IntPtr.Zero, IntPtr.Zero).ToInt64();
                        }
                    }
                    return true;
                }, IntPtr.Zero);
            }
            return true;
        }, IntPtr.Zero);
        if (state.Host != IntPtr.Zero) {
            EnumChildWindows(state.Host, (child, _) => {
                if (GetParent(child) != state.Host) return true;
                var className = new StringBuilder(64);
                GetClassName(child, className, className.Capacity);
                if (className.ToString() == "FLUTTER_RUNNER_WIN32_WINDOW") {
                    state.SessionWindows++;
                    if (IsWindowVisible(child)) state.VisibleSessionWindows++;
                }
                return true;
            }, IntPtr.Zero);
        }
        return state;
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

function Capture-ScreenRegion([IntPtr]$Window, [string]$Path) {
    $rect = New-Object FlutterHomeInput+Rect
    if (-not [FlutterHomeInput]::GetWindowRect($Window, [ref]$rect)) {
        throw 'Could not read the tab host bounds for tooltip capture.'
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object Drawing.Bitmap($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0,
            [Drawing.Size]::new($width, $height))
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
function Get-SessionTabCount {
    [FlutterHomeInput]::FindSessionTabs($ProcessId).Count
}

function Get-SessionTabState {
    [FlutterHomeInput]::FindSessionTabs($ProcessId)
}

if ($CloseSelectedTab) {
    if ($ExpectedSessionCount -lt 2) {
        throw 'Closing a selected tab requires at least two open sessions.'
    }
    $before = Get-SessionTabState
    if ($before.Count -ne $ExpectedSessionCount -or
        $before.SessionWindows -ne $ExpectedSessionCount -or
        $before.VisibleSessionWindows -ne 1 -or $before.Selected -lt 0) {
        throw "Cannot close the selected tab: expected $ExpectedSessionCount hosted sessions and one visible; found count=$($before.Count), windows=$($before.SessionWindows), visible=$($before.VisibleSessionWindows), selected=$($before.Selected)."
    }
    if ($before.Host -eq $homeWindow) {
        throw "The session tab host unexpectedly matches the home window ($homeWindow)."
    }
    $beforeLabels = @([FlutterHomeInput]::GetTabLabels($before.Tabs))
    $closedCaption = $beforeLabels[$before.Selected]
    if (-not [FlutterHomeInput]::ClickTabClose($before.Tabs, $before.Selected)) {
        throw "Could not click the selected tab's close glyph at index $($before.Selected)."
    }
    Capture-Window $before.Host (Join-Path $ArtifactDirectory 'flutter-session-tabs-before-close.png')
    Write-Output "Clicking selected tab close glyph: host=$($before.Host) tabs=$($before.Tabs) home=$homeWindow selected=$($before.Selected)."
    if ($ExpectCloseFailure) {
        $tracePath = Join-Path $ArtifactDirectory 'session-tabs.tsv'
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        $trace = ''
        $win32FailureObserved = $false
        do {
            Start-Sleep -Milliseconds 50
            $state = Get-SessionTabState
            if (Test-Path -LiteralPath $tracePath) {
                $trace = Get-Content -LiteralPath $tracePath -Raw
            }
            $win32FailureObserved = [regex]::IsMatch($trace,
                '(?m)^runner\tclose-tab-post-failed\t[^\t]*\t[^\t]*\t[^\t]*\t16\t1400\t')
        } while ((-not [regex]::IsMatch($trace, '(?m)^close-tab-test-inject-invalid-hwnd\t') -or
            -not [regex]::IsMatch($trace, '(?m)^close-tab-post-failed\t') -or
            -not [regex]::IsMatch($trace, '(?m)^close-tab-post-rollback\t') -or
            -not $win32FailureObserved) -and
            [DateTime]::UtcNow -lt $deadline)

        $processAlive = $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
        if (-not [regex]::IsMatch($trace, '(?m)^close-tab-test-inject-invalid-hwnd\t') -or
            -not [regex]::IsMatch($trace, '(?m)^close-tab-post-failed\t') -or
            -not [regex]::IsMatch($trace, '(?m)^close-tab-post-rollback\t') -or
            -not $win32FailureObserved -or
            $state.Count -ne $ExpectedSessionCount -or
            $state.SessionWindows -ne $ExpectedSessionCount -or
            $state.VisibleSessionWindows -ne 1 -or
            $state.Selected -ne $before.Selected -or -not $processAlive) {
            throw "Injected PostMessageW failure did not restore the selected live tab: before=$($before | ConvertTo-Json -Compress) after=$($state | ConvertTo-Json -Compress) process_alive=$processAlive; trace=$tracePath`n$trace"
        }
        Capture-Window $state.Host (Join-Path $ArtifactDirectory 'flutter-session-tabs-after-close-failure.png')
        $evidence = [ordered]@{
            scope = 'Windows Flutter loopback; real PostMessageW call against invalid HWND; live child remains valid'
            before = $before
            after = $state
            process_alive = $processAlive
            injection_observed = $true
            post_failure_observed = $true
            win32_error_code = 1400
            rollback_observed = $true
        }
        $evidence | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ArtifactDirectory 'flutter-session-tabs-close-failure.json') -Encoding UTF8
        Write-Output "PASS: real Win32 PostMessageW failure returned ERROR_INVALID_WINDOW_HANDLE (1400) and rolled back; $($state.Count) tabs remain, selected=$($state.Selected), visible=$($state.VisibleSessionWindows), process_alive=$processAlive."
        return
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 50
        $state = Get-SessionTabState
    } while (($state.Count -ne ($ExpectedSessionCount - 1) -or
        $state.VisibleSessionWindows -ne 1) -and [DateTime]::UtcNow -lt $deadline)
    if ($state.Count -ne ($ExpectedSessionCount - 1) -or
        $state.SessionWindows -ne ($ExpectedSessionCount - 1) -or
        $state.VisibleSessionWindows -ne 1) {
        $processAlive = $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
        $inventory = [FlutterHomeInput]::DescribeProcessWindows($ProcessId)
        $inventoryPath = Join-Path $ArtifactDirectory 'flutter-window-inventory-after-close-failure.txt'
        Set-Content -LiteralPath $inventoryPath -Value $inventory -Encoding UTF8
        throw "Closing the selected tab did not leave one active session: count=$($state.Count), windows=$($state.SessionWindows), visible=$($state.VisibleSessionWindows), process_alive=$processAlive; window inventory: $inventoryPath`n$inventory"
    }
    Start-Sleep -Milliseconds 150
    if ([FlutterHomeInput]::FindVisibleWindowText(
        $ProcessId, $closedCaption, 'ChastSessionTabTooltip') -eq $closedCaption) {
        throw "Closing tab '$closedCaption' left its stale tooltip visible."
    }
    Capture-Window $state.Host (Join-Path $ArtifactDirectory 'flutter-session-tabs-after-close.png')
    $newButton = [FlutterHomeInput]::GetDlgItem($state.Host, 4102)
    if ($newButton -eq [IntPtr]::Zero) {
        throw "Could not resolve new connection button 4102 under tab host $($state.Host)."
    }
    [void][FlutterHomeInput]::SendMessageW($newButton, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $homeDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not [FlutterHomeInput]::IsWindowVisible($homeWindow) -and
        [DateTime]::UtcNow -lt $homeDeadline) {
        Start-Sleep -Milliseconds 50
    }
    if (-not [FlutterHomeInput]::IsWindowVisible($homeWindow)) {
        throw 'Closing the selected tab did not return to the home window.'
    }
    Write-Output "Closed selected session tab; $($state.Count) tab(s) remain, and the home window is visible for reconnect."
    return
}

function Select-SessionTab([int]$Index) {
    $state = Get-SessionTabState
    if ($Index -lt 0 -or $Index -ge $state.Count) {
        throw "Session tab index $Index is outside the current tab range."
    }
    $rect = New-Object FlutterHomeInput+Rect
    if (-not [FlutterHomeInput]::GetTabItemRect($state.Tabs, $Index, [ref]$rect)) {
        throw "Could not read tab $Index bounds from the viewer process."
    }
    $x = [int](($rect.Left + $rect.Right) / 2)
    $y = [int](($rect.Top + $rect.Bottom) / 2)
    $before = $state
    $itemBounds = "$($rect.Left),$($rect.Top)-$($rect.Right),$($rect.Bottom)"
    Send-Click $state.Tabs $x $y
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        Start-Sleep -Milliseconds 50
        $state = Get-SessionTabState
    } while (($state.Selected -ne $Index -or $state.VisibleSessionWindows -ne 1) -and
        [DateTime]::UtcNow -lt $deadline)
    if ($state.Selected -ne $Index -or $state.VisibleSessionWindows -ne 1) {
        throw "Selecting tab $Index failed: before count=$($before.Count), selected=$($before.Selected), host=$($before.Host), tabs=$($before.Tabs), rect=$itemBounds; after count=$($state.Count), selected=$($state.Selected), host=$($state.Host), tabs=$($state.Tabs), session windows=$($state.SessionWindows), visible=$($state.VisibleSessionWindows)."
    }
    return $state
}

$initialTabCount = Get-SessionTabCount
if ($initialTabCount -ne ($ExpectedSessionCount - 1)) {
    throw "Expected $($ExpectedSessionCount - 1) session tab(s) before submission; found $initialTabCount."
}
Send-Click $viewHandle 616 309

$sessionTabCount = Get-SessionTabCount
$retryAt = [DateTime]::UtcNow.AddSeconds(4)
$deadline = [DateTime]::UtcNow.AddSeconds(14)
$retried = $false
while ($sessionTabCount -lt $ExpectedSessionCount -and [DateTime]::UtcNow -lt $deadline) {
    if (-not $retried -and [DateTime]::UtcNow -ge $retryAt) {
        if (-not $UseExistingCredentials) {
            Capture-Window $homeWindow (Join-Path $ArtifactDirectory 'flutter-home-after-submit.png')
        }
        Send-Click $viewHandle 616 309
        $retried = $true
    }
    Start-Sleep -Milliseconds 100
    $sessionTabCount = Get-SessionTabCount
}
if ($sessionTabCount -ne $ExpectedSessionCount) {
    if (-not $UseExistingCredentials) {
        Capture-Window $homeWindow (Join-Path $ArtifactDirectory 'flutter-home-after-submit.png')
    }
    throw "The Flutter connect action opened $sessionTabCount session tab(s); expected $ExpectedSessionCount."
}
if ($ExpectedSessionCount -gt 1) {
    $state = Get-SessionTabState
    if ($state.SessionWindows -ne $ExpectedSessionCount -or
        $state.VisibleSessionWindows -ne 1 -or
        $state.Selected -lt 0 -or $state.Selected -ge $ExpectedSessionCount) {
        throw "Expected $ExpectedSessionCount hosted session windows and exactly one visible; found $($state.SessionWindows) windows, $($state.VisibleSessionWindows) visible, selected=$($state.Selected)."
    }
    $tabLabels = @([FlutterHomeInput]::GetTabLabels($state.Tabs))
    if ($tabLabels.Count -ne $ExpectedSessionCount -or
        @($tabLabels | Select-Object -Unique).Count -ne $ExpectedSessionCount) {
        throw "Expected distinct accessible captions for all $ExpectedSessionCount session tabs; found: $($tabLabels -join ' | ')."
    }
    Write-Output "Verified distinct accessible session tab captions: $($tabLabels -join ' | ')."
    $savedCursor = New-Object FlutterHomeInput+Point
    if (-not [FlutterHomeInput]::GetCursorPos([ref]$savedCursor)) {
        throw 'Could not save the cursor position before the tooltip check.'
    }
    $hoverIndex = $state.Selected
    $hoverRect = New-Object FlutterHomeInput+Rect
    if (-not [FlutterHomeInput]::GetTabItemRect($state.Tabs, $hoverIndex, [ref]$hoverRect)) {
        throw "Could not resolve tab $hoverIndex for the tooltip check."
    }
    $hoverPoint = New-Object FlutterHomeInput+Point
    $hoverPoint.X = [int](($hoverRect.Left + $hoverRect.Right) / 2)
    $hoverPoint.Y = [int](($hoverRect.Top + $hoverRect.Bottom) / 2)
    $hoverClientPoint = New-Object FlutterHomeInput+Point
    $hoverClientPoint.X = $hoverPoint.X
    $hoverClientPoint.Y = $hoverPoint.Y
    if (-not [FlutterHomeInput]::ClientToScreen($state.Tabs, [ref]$hoverPoint)) {
        throw 'Could not translate the tab caption to screen coordinates.'
    }
    if (-not [FlutterHomeInput]::SetCursorPos($hoverPoint.X, $hoverPoint.Y)) {
        throw 'Could not move the cursor over the tab caption for its tooltip check.'
    }
    $packedPoint = [IntPtr]([int64]$hoverClientPoint.X -bor
        ([int64]$hoverClientPoint.Y -shl 16))
    [void][FlutterHomeInput]::SendMessageW(
        $state.Tabs, 0x0200, [IntPtr]::Zero, $packedPoint)
    try {
        $tooltipText = ''
        $tooltipWindow = [IntPtr]::Zero
        $tooltipDeadline = [DateTime]::UtcNow.AddSeconds(3)
        do {
            Start-Sleep -Milliseconds 100
            $tooltipWindow = [FlutterHomeInput]::FindVisibleWindowHandle(
                $ProcessId, $tabLabels[$hoverIndex], 'ChastSessionTabTooltip')
            $tooltipText = [FlutterHomeInput]::FindVisibleWindowText(
                $ProcessId, $tabLabels[$hoverIndex], 'ChastSessionTabTooltip')
        } while (($tooltipWindow -eq [IntPtr]::Zero -or
            [string]::IsNullOrEmpty($tooltipText)) -and
            [DateTime]::UtcNow -lt $tooltipDeadline)
        if ($tooltipText -ne $tabLabels[$hoverIndex]) {
            Capture-Window $state.Host (Join-Path $ArtifactDirectory 'flutter-session-tabs-tooltip-probe.png')
        } else {
            # Give the compositor time to present the popup before capturing
            # both the popup bounds and host region; PrintWindow does not
            # include owned overlays.
            Start-Sleep -Milliseconds 500
            Capture-ScreenRegion $tooltipWindow (Join-Path $ArtifactDirectory 'flutter-session-tabs-tooltip-popup.png')
            Capture-ScreenRegion $state.Host (Join-Path $ArtifactDirectory 'flutter-session-tabs-tooltip-visible.png')
        }
    } finally {
        [void][FlutterHomeInput]::SetCursorPos($savedCursor.X, $savedCursor.Y)
    }
    if ($tooltipText -ne $tabLabels[$hoverIndex]) {
        $windowUnderCursor = [FlutterHomeInput]::WindowFromPoint($hoverPoint)
        throw "Expected the hovered tab tooltip '$($tabLabels[$hoverIndex])'; got '$tooltipText'; probe=($($hoverPoint.X),$($hoverPoint.Y)) windowUnderCursor=$windowUnderCursor."
    }
    Write-Output "Verified hovered tab tooltip text: $tooltipText."
    Start-Sleep -Milliseconds 250
    $hiddenTooltipText = [FlutterHomeInput]::FindVisibleWindowText(
        $ProcessId, $tabLabels[$hoverIndex], 'ChastSessionTabTooltip')
    if ($hiddenTooltipText -eq $tabLabels[$hoverIndex]) {
        throw 'The tab tooltip remained visible after moving the pointer away.'
    }
    Write-Output 'Verified tab tooltip hides after pointer leaves the tab.'
    $hostWindow = $state.Host
    $firstIndex = $state.Selected
    $otherIndex = ($firstIndex + 1) % $ExpectedSessionCount
    Capture-Window $hostWindow (Join-Path $ArtifactDirectory 'flutter-session-tabs-before-switch.png')
    $state = Select-SessionTab $otherIndex
    Capture-Window $hostWindow (Join-Path $ArtifactDirectory 'flutter-session-tabs-after-switch.png')
    $state = Select-SessionTab $firstIndex
    Capture-Window $hostWindow (Join-Path $ArtifactDirectory 'flutter-session-tabs-after-switch-back.png')
    Write-Output "Verified session tab switching: selected $firstIndex -> $otherIndex -> $firstIndex; one of $($state.SessionWindows) session windows was visible at each step."
}
Write-Output "Submitted Flutter connection for $PeerId through HWND $viewHandle; $ExpectedSessionCount session tab(s) are open."
