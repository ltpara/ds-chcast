param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [Parameter(Mandatory = $true)][string]$TracePath,
    [switch]$MultiSessionTabs,
    [switch]$HoldBackgroundSession,
    [string]$BackgroundStatusPath,
    [switch]$VerifyAudioIsolation,
    [string]$AudioStatusPathA,
    [string]$AudioStatusPathB,
    [switch]$VerifyDisplayWindowLifecycle,
    [string]$DisplayDisconnectMarkerPath,
    [switch]$VerifyKeyboardShortcuts,
    [string]$PreferencesPath,
    [string]$PeerPreferencesKey
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class NativeFlutterInputUi {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);
    private delegate bool EnumChildWindowsCallback(IntPtr window, IntPtr parameter);
    public struct SessionTabState {
        public IntPtr Host;
        public IntPtr Tabs;
        public int Count;
        public int Selected;
    }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumChildWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")]
    private static extern int GetClassName(IntPtr window, StringBuilder className, int maxCount);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetWindowTextW")]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int maxCount);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);
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
    public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extraInfo);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint attachThread, uint attachToThread, bool attach);
    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")]
    public static extern void SwitchToThisWindow(IntPtr window, bool altTab);

    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT {
        public uint type;
        public InputUnion data;
    }
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx, dy;
        public uint mouseData, dwFlags, time;
        public UIntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk, wScan;
        public uint dwFlags, time;
        public UIntPtr dwExtraInfo;
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
    public static SessionTabState FindSessionTabs(int ownerProcessId) {
        var state = new SessionTabState {
            Host = IntPtr.Zero, Tabs = IntPtr.Zero, Count = 0, Selected = -1
        };
        EnumWindows((window, _) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == (uint)ownerProcessId) {
                EnumChildWindows(window, (child, __) => {
                    var className = new StringBuilder(64);
                    GetClassName(child, className, className.Capacity);
                    if (className.ToString() == "SysTabControl32") {
                        int count = (int)SendMessageW(child, 0x1304,
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
        return state;
    }
    public static long[] FindSessionWindows(IntPtr host) {
        var windows = new List<long>();
        EnumChildWindows(host, (child, _) => {
            if (GetParent(child) != host) return true;
            var className = new StringBuilder(64);
            GetClassName(child, className, className.Capacity);
            if (className.ToString() == "FLUTTER_RUNNER_WIN32_WINDOW")
                windows.Add(child.ToInt64());
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }
    public static long[] FindDisplayWindows(int ownerProcessId) {
        var windows = new List<long>();
        EnumWindows((window, _) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId != (uint)ownerProcessId) return true;
            var className = new StringBuilder(64);
            GetClassName(window, className, className.Capacity);
            if (className.ToString() != "FLUTTER_RUNNER_WIN32_WINDOW") return true;
            var title = new StringBuilder(256);
            GetWindowText(window, title, title.Capacity);
            if (title.ToString().StartsWith("Chast - Display", StringComparison.Ordinal))
                windows.Add(window.ToInt64());
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }
    public static long FindFlutterViewWindow(IntPtr parent) {
        long view = 0;
        EnumChildWindows(parent, (child, _) => {
            var className = new StringBuilder(64);
            GetClassName(child, className, className.Capacity);
            if (className.ToString() == "FLUTTERVIEW") {
                view = child.ToInt64();
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return view;
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
    public static bool BringToForeground(IntPtr window) {
        if (GetForegroundWindow() == window) {
            return true;
        }
        ShowWindow(window, 9);
        var foreground = GetForegroundWindow();
        uint foregroundProcessId;
        var foregroundThread = GetWindowThreadProcessId(foreground, out foregroundProcessId);
        var currentThread = GetCurrentThreadId();
        var attached = false;
        try {
            if (foregroundThread != 0 && foregroundThread != currentThread) {
                attached = AttachThreadInput(currentThread, foregroundThread, true);
            }
            BringWindowToTop(window);
            var broughtForward = SetForegroundWindow(window);
            if (!broughtForward && GetForegroundWindow() != window) {
                keybd_event(0x12, 0, 0, UIntPtr.Zero);
                try {
                    SwitchToThisWindow(window, true);
                } finally {
                    keybd_event(0x12, 0, 2, UIntPtr.Zero);
                }
            }
            return GetForegroundWindow() == window;
        } finally {
            if (attached) {
                AttachThreadInput(currentThread, foregroundThread, false);
            }
        }
    }
    public static void MouseButton(uint flags) {
        var input = new INPUT { type = 0 };
        input.data.mi.dwFlags = flags;
        input.data.mi.dwExtraInfo = UIntPtr.Zero;
        if (SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT))) != 1) {
            throw new InvalidOperationException("SendInput mouse failed: " + Marshal.GetLastWin32Error());
        }
    }
    public static void Key(ushort virtualKey, ushort scanCode, uint flags) {
        var input = new INPUT { type = 1 };
        input.data.ki.wVk = virtualKey;
        input.data.ki.wScan = scanCode;
        input.data.ki.dwFlags = flags;
        input.data.ki.dwExtraInfo = UIntPtr.Zero;
        if (SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT))) != 1) {
            throw new InvalidOperationException("SendInput key failed: " + Marshal.GetLastWin32Error());
        }
    }
}
'@

function Capture-Window([IntPtr]$Window, [string]$Path) {
    $rect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($Window, [ref]$rect)) {
        throw 'Could not read the Flutter session window bounds.'
    }
    $bitmap = New-Object Drawing.Bitmap(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [NativeFlutterInputUi]::PrintWindow($Window, $dc, 2)) {
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
    [void][NativeFlutterInputUi]::SendMessageW($Window, 0x0200, [IntPtr]::Zero, $point)
    [void][NativeFlutterInputUi]::SendMessageW($Window, 0x0201, [IntPtr]1, $point)
    [void][NativeFlutterInputUi]::SendMessageW($Window, 0x0202, [IntPtr]::Zero, $point)
}

function Send-PhysicalClick([int]$X, [int]$Y) {
    if (-not [NativeFlutterInputUi]::SetCursorPos($X, $Y)) {
        throw "Could not move the pointer to screen coordinate ($X, $Y)."
    }
    Start-Sleep -Milliseconds 100
    [NativeFlutterInputUi]::MouseButton(0x0002)
    try {
        Start-Sleep -Milliseconds 80
    } finally {
        [NativeFlutterInputUi]::MouseButton(0x0004)
    }
}

function Select-SessionTabIndex([int]$Index) {
    $state = [NativeFlutterInputUi]::FindSessionTabs($ProcessId)
    if ($Index -lt 0 -or $Index -ge $state.Count) {
        throw "Session tab index $Index is outside the current tab range."
    }
    if ($state.Selected -eq $Index) { return $state }
    $rect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetTabItemRect($state.Tabs, $Index, [ref]$rect)) {
        throw "Could not read tab $Index bounds."
    }
    Send-Click $state.Tabs ([int](($rect.Left + $rect.Right) / 2)) `
        ([int](($rect.Top + $rect.Bottom) / 2))
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        Start-Sleep -Milliseconds 50
        $state = [NativeFlutterInputUi]::FindSessionTabs($ProcessId)
    } while ($state.Selected -ne $Index -and [DateTime]::UtcNow -lt $deadline)
    if ($state.Selected -ne $Index) {
        throw "Selecting tab $Index failed; selected=$($state.Selected)."
    }
    return $state
}

function Get-DisplayWindows {
    @([NativeFlutterInputUi]::FindDisplayWindows($ProcessId))
}

function Get-SessionRenderTarget([int]$Index) {
    $state = Select-SessionTabIndex $Index
    $visible = @([NativeFlutterInputUi]::FindSessionWindows($state.Host) |
        Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
    if ($visible.Count -ne 1) {
        throw "Tab $Index does not have exactly one visible session window."
    }
    $session = [IntPtr]([int64]$visible[0])
    $view = [IntPtr]([NativeFlutterInputUi]::FindFlutterViewWindow($session))
    $clientRect = New-Object NativeFlutterInputUi+Rect
    if ($view -eq [IntPtr]::Zero -or
        -not [NativeFlutterInputUi]::GetClientRect($view, [ref]$clientRect)) {
        throw "Could not locate the render view for tab $Index."
    }
    return [pscustomobject]@{
        Session = $session
        View = $view
        Width = $clientRect.Right - $clientRect.Left
        Height = $clientRect.Bottom - $clientRect.Top
    }
}

function Open-SecondaryDisplayWindow(
    [int]$TabIndex,
    [int]$ExpectedWindowCount,
    [string]$ScreenshotName
) {
    $target = Get-SessionRenderTarget $TabIndex
    if (-not [NativeFlutterInputUi]::BringToForeground($captureHandle)) {
        throw "Could not focus the tab host before opening display window $TabIndex."
    }
    $hover = [IntPtr]([int64]([int]($target.Width / 2)) -bor ([int64]20 -shl 16))
    [void][NativeFlutterInputUi]::SendMessageW($target.View, 0x0200, [IntPtr]::Zero, $hover)
    Start-Sleep -Milliseconds 350
    $before = @(Get-DisplayWindows)
    $viewRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($target.View, [ref]$viewRect)) {
        throw "Could not read the render bounds for tab $TabIndex."
    }
    $buttonX = [int][Math]::Round($target.Width * 0.075)
    Send-PhysicalClick ($viewRect.Left + $buttonX) ($viewRect.Top + 20)
    Start-Sleep -Milliseconds 350
    Capture-Window $captureHandle (Join-Path $ArtifactDirectory $ScreenshotName)

    # The Flutter PopupMenu is rendered inside FLUTTERVIEW and does not expose
    # menu items through UI Automation; click the final display action directly.
    Send-PhysicalClick ($viewRect.Left + [int][Math]::Round($target.Width * 0.15)) `
        ($viewRect.Top + [int][Math]::Round($target.Height * 0.64))

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $current = @(Get-DisplayWindows)
    } while ($current.Count -lt $ExpectedWindowCount -and
        [DateTime]::UtcNow -lt $deadline)
    if ($current.Count -ne $ExpectedWindowCount) {
        throw "Opening display 2 from tab $TabIndex produced $($current.Count) display window(s), expected $ExpectedWindowCount. Inspect $ScreenshotName."
    }
    $created = @($current | Where-Object { $before -notcontains $_ })
    if ($created.Count -ne 1) {
        throw "Could not identify the display window created by tab $TabIndex; new handles=$($created -join ',')."
    }
    return [IntPtr]([int64]$created[0])
}

function Assert-OnlyDisplayVisible([IntPtr]$ExpectedWindow, [IntPtr]$OtherWindow) {
    if (-not [NativeFlutterInputUi]::IsWindow($ExpectedWindow) -or
        -not [NativeFlutterInputUi]::IsWindow($OtherWindow) -or
        -not [NativeFlutterInputUi]::IsWindowVisible($ExpectedWindow) -or
        [NativeFlutterInputUi]::IsWindowVisible($OtherWindow)) {
        throw "Display visibility does not match the selected parent session (expected=$ExpectedWindow, other=$OtherWindow)."
    }
}

function Find-ToolbarKeyboardButton(
    [IntPtr]$SessionWindow,
    [IntPtr]$ViewWindow,
    [string]$ArtifactDirectory
) {
    $automationRoot = [Windows.Automation.AutomationElement]::FromHandle($SessionWindow)
    $elements = $automationRoot.FindAll(
        [Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    $accessibleButton = $null
    $remoteControlEnabled = $false
    $accessibleNames = New-Object System.Collections.Generic.List[string]
    foreach ($element in $elements) {
        $name = [string]$element.Current.Name
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            $accessibleNames.Add($name)
        }
        if ($name -match '(?i)关闭远程键鼠|disable remote (input|keyboard|mouse|control)') {
            $accessibleButton = $element
            $remoteControlEnabled = $true
            break
        }
        if ($name -match '(?i)开启远程键鼠|enable remote (input|keyboard|mouse|control)') {
            $accessibleButton = $element
            break
        }
    }
    if ($accessibleButton -ne $null) {
        $viewRect = New-Object NativeFlutterInputUi+Rect
        if (-not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
            throw 'Could not read the Flutter render window position.'
        }
        $point = $accessibleButton.GetClickablePoint()
        return [pscustomobject]@{
            ClientX = [int][Math]::Round($point.X - $viewRect.Left)
            ClientY = [int][Math]::Round($point.Y - $viewRect.Top)
            Name = [string]$accessibleButton.Current.Name
            Element = $accessibleButton
            RemoteControlEnabled = $remoteControlEnabled
        }
    }
    $nameSummary = @($accessibleNames | Select-Object -First 24) -join ' | '

    $screenshotPath = Join-Path $ArtifactDirectory 'flutter-input-toolbar.png'
    Capture-Window $SessionWindow $screenshotPath

    $sessionRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($SessionWindow, [ref]$sessionRect)) {
        throw 'Could not read the Flutter session window position.'
    }
    $viewRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
        throw 'Could not read the Flutter render window position.'
    }
    $offsetX = $viewRect.Left - $sessionRect.Left
    $toolbarTop = [Math]::Max(0, $viewRect.Top - $sessionRect.Top)

    $bitmap = [Drawing.Bitmap]::FromFile($screenshotPath)
    try {
        $scanTop = $toolbarTop
        $scanBottom = [Math]::Min($bitmap.Height, $toolbarTop + 48)
        $columns = New-Object System.Collections.Generic.List[int]
        for ($x = 0; $x -lt $bitmap.Width; $x++) {
            $iconPixels = 0
            for ($y = $scanTop; $y -lt $scanBottom; $y++) {
                $color = $bitmap.GetPixel($x, $y)
                $grayIcon = ([Math]::Max($color.R, [Math]::Max($color.G, $color.B)) -gt 80) -and
                    ([Math]::Abs($color.R - $color.G) -lt 25) -and
                    ([Math]::Abs($color.G - $color.B) -lt 25)
                $accentIcon = $color.R -le 100 -and $color.G -ge 90 -and $color.B -ge 180
                if ($grayIcon -or $accentIcon) {
                    $iconPixels++
                }
            }
            if ($iconPixels -ge 2) {
                $columns.Add($x)
            }
        }
    } finally {
        $bitmap.Dispose()
    }

    $runs = New-Object System.Collections.Generic.List[object]
    if ($columns.Count -gt 0) {
        $start = $columns[0]
        $previous = $columns[0]
        for ($i = 1; $i -lt $columns.Count; $i++) {
            $current = $columns[$i]
            if ($current -gt $previous + 1) {
                $runs.Add([pscustomobject]@{ Start = $start; End = $previous })
                $start = $current
            }
            $previous = $current
        }
        $runs.Add([pscustomobject]@{ Start = $start; End = $previous })
    }

    $icons = @($runs | Where-Object {
        ($_.End - $_.Start + 1) -ge 4 -and
        (($_.Start + $_.End) / 2) -lt 360
    })
    if ($icons.Count -lt 8) {
        throw "Could not locate the remote keyboard toolbar button; found $($icons.Count) left-side toolbar icons. UIA names: $nameSummary"
    }

    # The keyboard control follows the mouse-options button in the rendered toolbar.
    $keyboard = $icons[7]
    $imageX = [int](($keyboard.Start + $keyboard.End) / 2)
    $bitmap = [Drawing.Bitmap]::FromFile($screenshotPath)
    try {
        $accentPixels = 0
        for ($x = $keyboard.Start; $x -le $keyboard.End; $x++) {
            for ($y = $toolbarTop; $y -lt [Math]::Min($bitmap.Height, $toolbarTop + 48); $y++) {
                $color = $bitmap.GetPixel($x, $y)
                if ($color.R -le 100 -and $color.G -ge 90 -and $color.B -ge 180) {
                    $accentPixels++
                }
            }
        }
    } finally {
        $bitmap.Dispose()
    }
    return [pscustomobject]@{
        ImageX = $imageX
        ImageY = $toolbarTop + 32
        ClientX = $imageX - $offsetX
        ClientY = 32
        Screenshot = $screenshotPath
        Name = "rendered toolbar icon 8 (accent pixels=$accentPixels)"
        Element = $null
        RemoteControlEnabled = $accentPixels -ge 5
    }
}

function Find-ToolbarAudioButton(
    [IntPtr]$SessionWindow,
    [IntPtr]$ViewWindow
) {
    $automationRoot = [Windows.Automation.AutomationElement]::FromHandle($SessionWindow)
    $elements = $automationRoot.FindAll(
        [Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    $audioButtonPattern = '(?i)^(静音（长按选择输出设备）|取消静音（长按选择输出设备）|Mute \(long-press to pick output device\)|Unmute \(long-press to pick output device\))$'
    foreach ($element in $elements) {
        $name = [string]$element.Current.Name
        if ($name -notmatch $audioButtonPattern) { continue }
        $viewRect = New-Object NativeFlutterInputUi+Rect
        if (-not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
            throw 'Could not read the Flutter render window position for audio input.'
        }
        $point = $element.GetClickablePoint()
        return [pscustomobject]@{
            ClientX = [int][Math]::Round($point.X - $viewRect.Left)
            ClientY = [int][Math]::Round($point.Y - $viewRect.Top)
            Name = $name
            Element = $element
        }
    }
    throw 'Could not locate the audio toolbar button by its accessible label.'
}

function Test-ToolbarExpanded([string]$ScreenshotPath, [int]$ToolbarTop) {
    $bitmap = [Drawing.Bitmap]::FromFile($ScreenshotPath)
    try {
        $scanTop = $ToolbarTop
        $scanBottom = [Math]::Min($bitmap.Height, $ToolbarTop + 48)
        $columns = New-Object System.Collections.Generic.List[int]
        $scanLeft = [int]($bitmap.Width * 0.2)
        $scanRight = [int]($bitmap.Width * 0.8)
        for ($x = $scanLeft; $x -lt $scanRight; $x++) {
            $iconPixels = 0
            for ($y = $scanTop; $y -lt $scanBottom; $y++) {
                $color = $bitmap.GetPixel($x, $y)
                $grayIcon = ([Math]::Max($color.R, [Math]::Max($color.G, $color.B)) -gt 80) -and
                    ([Math]::Abs($color.R - $color.G) -lt 25) -and
                    ([Math]::Abs($color.G - $color.B) -lt 25)
                $accentIcon = $color.R -le 100 -and $color.G -ge 90 -and $color.B -ge 180
                if ($grayIcon -or $accentIcon) {
                    $iconPixels++
                }
            }
            if ($iconPixels -ge 2) {
                $columns.Add($x)
            }
        }
    } finally {
        $bitmap.Dispose()
    }
    if ($columns.Count -eq 0) {
        return $false
    }

    $iconCount = 0
    $start = $columns[0]
    $previous = $columns[0]
    for ($i = 1; $i -le $columns.Count; $i++) {
        $ended = $i -eq $columns.Count
        if (-not $ended) {
            $current = $columns[$i]
            $ended = $current -gt $previous + 1
        }
        if ($ended) {
            $center = ($start + $previous) / 2
            if (($previous - $start + 1) -ge 4) {
                $iconCount++
            }
            if ($i -lt $columns.Count) {
                $start = $columns[$i]
                $previous = $columns[$i]
            }
        } else {
            $previous = $columns[$i]
        }
    }
    return $iconCount -ge 7
}

function Get-RecordingToolbarAccentPixels(
    [string]$ScreenshotPath,
    [IntPtr]$CaptureWindow,
    [IntPtr]$ViewWindow
) {
    $captureRect = New-Object NativeFlutterInputUi+Rect
    $viewRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($CaptureWindow, [ref]$captureRect) -or
        -not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
        throw 'Could not read the recording toolbar screenshot bounds.'
    }

    $toolbarTop = [Math]::Max(0, $viewRect.Top - $captureRect.Top)
    $bitmap = [Drawing.Bitmap]::FromFile($ScreenshotPath)
    try {
        $scanLeft = [Math]::Max(0, [int][Math]::Floor($bitmap.Width * 0.82))
        $scanRight = [Math]::Min($bitmap.Width, [int][Math]::Ceiling($bitmap.Width * 0.85))
        $scanTop = $toolbarTop
        $scanBottom = [Math]::Min($bitmap.Height, $toolbarTop + 48)
        if ($scanRight -le $scanLeft -or $scanBottom -le $scanTop) {
            throw "Recording toolbar scan region is outside the screenshot: $ScreenshotPath"
        }

        $accentPixels = 0
        for ($x = $scanLeft; $x -lt $scanRight; $x++) {
            for ($y = $scanTop; $y -lt $scanBottom; $y++) {
                $color = $bitmap.GetPixel($x, $y)
                if ($color.R -le 100 -and $color.G -ge 90 -and $color.B -ge 180) {
                    $accentPixels++
                }
            }
        }
        return $accentPixels
    } finally {
        $bitmap.Dispose()
    }
}

function Read-TraceEvents([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $events = @()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $events += ($line | ConvertFrom-Json)
    }
    return $events
}

function Read-BackgroundStatus([string]$Path) {
    try {
        $line = [IO.File]::ReadAllText($Path).Trim()
        $fields = $line.Split([char]9, 8)
        if ($fields.Length -ne 8) { return $null }
        return [pscustomobject]@{
            State = [uint64]$fields[0]
            Received = [uint64]$fields[2]
            Decoded = [uint64]$fields[3]
            Published = [uint64]$fields[4]
            Recording = [uint64]$fields[5]
            RecordSamples = [uint64]$fields[6]
            RecordPath = $fields[7]
        }
    } catch {
        return $null
    }
}

function Read-AudioStatus([string]$Path) {
    try {
        $fields = [IO.File]::ReadAllText($Path).Trim().Split([char]9, 12)
        if ($fields.Length -ne 12) { return $null }
        return [pscustomobject]@{
            State = [uint64]$fields[0]
            Received = [uint64]$fields[2]
            Decoded = [uint64]$fields[3]
            AudioEnabled = [uint64]$fields[8]
            AudioMuted = [uint64]$fields[9]
            AudioReceived = [uint64]$fields[10]
            AudioDecoded = [uint64]$fields[11]
        }
    } catch {
        return $null
    }
}

function Wait-AudioStatus([string]$Path, [uint64]$Muted, [uint64]$DecodedAfter = 0) {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $snapshot = Read-AudioStatus $Path
        if ($null -ne $snapshot -and $snapshot.State -eq 3 -and
            $snapshot.AudioEnabled -eq 1 -and $snapshot.AudioMuted -eq $Muted -and
            $snapshot.AudioDecoded -gt $DecodedAfter) {
            return $snapshot
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Audio session did not reach muted=$Muted with advancing decode state: $Path"
}

function Invoke-AudioToggle(
    [IntPtr]$SessionWindow,
    [IntPtr]$ViewWindow,
    [int]$Width,
    [string]$ArtifactPath
) {
    $screenshot = Join-Path $ArtifactDirectory $ArtifactPath
    Capture-Window $captureHandle $screenshot
    try {
        $button = Find-ToolbarAudioButton $SessionWindow $ViewWindow
        Invoke-ToolbarKeyboardButton $button $ViewWindow
        $button | Add-Member -NotePropertyName ClickMode -NotePropertyValue 'UIA accessible button'
    } catch {
        $viewRect = New-Object NativeFlutterInputUi+Rect
        if (-not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
            throw 'Could not read the active Flutter view bounds for the audio fallback.'
        }
        $x = [int][Math]::Round($Width * 0.583)
        Send-PhysicalClick ($viewRect.Left + $x) ($viewRect.Top + 20)
        $button = [pscustomobject]@{
            Name = "screen-coordinate fallback x=$x"
            ClickMode = 'screen-coordinate fallback'
        }
    }
    return $button
}

function Wait-BackgroundStatus([string]$Path, [int]$TimeoutSeconds = 10) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $snapshot = Read-BackgroundStatus $Path
        if ($null -ne $snapshot -and $snapshot.State -eq 3 -and
            $snapshot.Recording -eq 1 -and $snapshot.RecordSamples -gt 0) {
            return $snapshot
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Background session did not reach streaming and recording state: $Path"
}

function Wait-Trace([scriptblock]$Predicate, [int]$TimeoutSeconds, [string]$Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $events = @(Read-TraceEvents $TracePath)
        if (& $Predicate $events) { return $events }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for native input trace: $Description ($TracePath)"
}

function Get-StatsPreference {
    if ([string]::IsNullOrWhiteSpace($PreferencesPath) -or
        [string]::IsNullOrWhiteSpace($PeerPreferencesKey)) {
        throw 'Keyboard shortcut verification requires the isolated preferences path and peer key.'
    }
    $document = Get-Content -LiteralPath $PreferencesPath -Raw | ConvertFrom-Json
    $peerProperty = $document.peers.PSObject.Properties[$PeerPreferencesKey]
    if ($null -eq $peerProperty) {
        throw "The isolated preferences do not contain peer '$PeerPreferencesKey'."
    }
    $peer = $peerProperty.Value
    $bindings = @($peer.view_remote_shortcuts)
    if ($bindings.Count -ne 2 -or
        -not (@($bindings | Where-Object {
            $_.action -eq 'toggleControl' -and $_.keyUsage -eq 0x40 -and $_.modifiers -eq 1
        }).Count -eq 1) -or
        -not (@($bindings | Where-Object {
            $_.action -eq 'toggleStats' -and $_.keyUsage -eq 0x41 -and $_.modifiers -eq 1
        }).Count -eq 1)) {
        throw 'The isolated peer preferences do not contain the expected Ctrl+F7/F8 bindings.'
    }
    return [bool]$peer.view_stats
}

function Wait-StatsPreference([bool]$Expected) {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        if ((Get-StatsPreference) -eq $Expected) { return }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "The stats preference did not change to $Expected after Ctrl+F8."
}

function Send-Shortcut([UInt16]$VirtualKey, [UInt16]$ScanCode) {
    [NativeFlutterInputUi]::Key(0xA2, 0x1D, 0)
    try {
        Start-Sleep -Milliseconds 80
        [NativeFlutterInputUi]::Key($VirtualKey, $ScanCode, 0)
        try {
            Start-Sleep -Milliseconds 80
        } finally {
            [NativeFlutterInputUi]::Key($VirtualKey, $ScanCode, 0x0002)
        }
        Start-Sleep -Milliseconds 80
    } finally {
        [NativeFlutterInputUi]::Key(0xA2, 0x1D, 0x0002)
    }
}

function Send-TestA([bool]$Down) {
    $flags = if ($Down) { 0 } else { 0x0002 }
    [NativeFlutterInputUi]::Key(0x41, 0x1E, [uint32]$flags)
}

function Get-AReleaseSequence(
    [object[]]$Items,
    [int]$ReceivedBefore,
    [int]$QueuedBefore,
    [int]$ReleaseAllBefore
) {
    $received = @($Items | Where-Object {
        $_.kind -eq 'release_control' -and $_.stage -eq 'input_release_received'
    }).Count -gt $ReceivedBefore
    $queued = @($Items | Where-Object {
        $_.kind -eq 'release_control' -and $_.stage -eq 'release_held_queued'
    }).Count -gt $QueuedBefore

    $releaseAllIndex = -1
    $releaseAllNumber = 0
    for ($i = 0; $i -lt $Items.Count; $i++) {
        if ($Items[$i].kind -eq 'release_control' -and
            $Items[$i].stage -eq 'release_all_begin') {
            $releaseAllNumber++
            if ($releaseAllNumber -gt $ReleaseAllBefore) {
                $releaseAllIndex = $i
                break
            }
        }
    }

    $releaseAllEndIndex = -1
    $aUpIndex = -1
    if ($releaseAllIndex -ge 0) {
        for ($i = $releaseAllIndex + 1; $i -lt $Items.Count; $i++) {
            if ($Items[$i].kind -eq 'release_control' -and
                $Items[$i].stage -eq 'release_all_end') {
                $releaseAllEndIndex = $i
                break
            }
            if ($Items[$i].kind -eq 'injected' -and
                $Items[$i].input_type -eq 1 -and
                $Items[$i].scan_code -eq 30 -and
                ($Items[$i].flags -band 2) -ne 0 -and
                $aUpIndex -lt 0) {
                $aUpIndex = $i
            }
        }
    }

    return [pscustomobject]@{
        Received = $received
        Queued = $queued
        ReleaseAllStarted = $releaseAllIndex -ge 0
        ReleaseAllCompleted = $releaseAllEndIndex -ge 0
        InjectedAUp = $aUpIndex -ge 0 -and
            $releaseAllEndIndex -gt $aUpIndex
        ReleaseAllIndex = $releaseAllIndex
        AUpIndex = $aUpIndex
        ReleaseAllEndIndex = $releaseAllEndIndex
    }
}

function Show-Toolbar(
    [IntPtr]$SessionWindow,
    [IntPtr]$ViewWindow,
    [int]$Width,
    [string]$ArtifactDirectory
) {
    $point = [IntPtr]([int64]([int]([Math]::Floor($Width / 2))) -bor ([int64]20 -shl 16))
    [void][NativeFlutterInputUi]::SendMessageW($ViewWindow, 0x0200, [IntPtr]::Zero, $point)
    Start-Sleep -Milliseconds 300

    $probe = Join-Path $ArtifactDirectory 'flutter-input-toolbar-probe.png'
    Capture-Window $SessionWindow $probe
    $sessionRect = New-Object NativeFlutterInputUi+Rect
    $viewRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($SessionWindow, [ref]$sessionRect) -or
        -not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
        throw 'Could not read the toolbar window positions.'
    }
    $toolbarTop = [Math]::Max(0, $viewRect.Top - $sessionRect.Top)
    if (Test-ToolbarExpanded $probe $toolbarTop) {
        return
    }

    # The collapsed bar's centre is its expand control. Only click it after
    # the probe confirms that the full toolbar is not already open.
    $viewRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
        throw 'Could not read the Flutter render window position for toolbar expansion.'
    }
    Send-PhysicalClick ($viewRect.Left + [int]([Math]::Floor($Width / 2))) `
        ($viewRect.Top + 22)
    Start-Sleep -Milliseconds 700
    $expanded = Join-Path $ArtifactDirectory 'flutter-input-toolbar-expanded.png'
    Capture-Window $SessionWindow $expanded
    if (-not (Test-ToolbarExpanded $expanded $toolbarTop)) {
        throw "The remote toolbar did not expand after clicking its center control; inspect $expanded."
    }
}

function Invoke-ToolbarKeyboardButton([object]$Button, [IntPtr]$ViewWindow) {
    $pattern = $null
    if ($null -ne $Button.Element -and $Button.Element.TryGetCurrentPattern(
            [Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
        $pattern.Invoke()
        return
    }
    $viewRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($ViewWindow, [ref]$viewRect)) {
        throw 'Could not read the Flutter render window position for toolbar input.'
    }
    Send-PhysicalClick ($viewRect.Left + $Button.ClientX) `
        ($viewRect.Top + $Button.ClientY)
}

function Wait-ToolbarRemoteControlState(
    [IntPtr]$ViewWindow,
    [bool]$Enabled,
    [int]$TimeoutSeconds = 5
) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastState = 'button not found'
    do {
        try {
            $button = Find-ToolbarKeyboardButton $captureHandle $ViewWindow $ArtifactDirectory
            $lastState = "'$($button.Name)' (enabled=$($button.RemoteControlEnabled))"
            if ($button.RemoteControlEnabled -eq $Enabled) {
                return $button
            }
        } catch {
            $lastState = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for remote control enabled=$Enabled; current state: $lastState"
}

New-Item -ItemType Directory -Path $ArtifactDirectory -Force | Out-Null
if ($HoldBackgroundSession -and [string]::IsNullOrWhiteSpace($BackgroundStatusPath)) {
    throw '-HoldBackgroundSession requires -BackgroundStatusPath.'
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
$tabState = $null
$backgroundSessionHandle = [IntPtr]::Zero
if ($MultiSessionTabs) {
    $tabState = [NativeFlutterInputUi]::FindSessionTabs($ProcessId)
    if ($tabState.Count -eq 2 -and $tabState.Selected -ne 0) {
        $tabState = Select-SessionTabIndex 0
    }
    $sessionWindows = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host))
    $visibleWindows = @($sessionWindows | Where-Object {
        [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_))
    })
    $hiddenWindows = @($sessionWindows | Where-Object {
        -not [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_))
    })
    if ($tabState.Count -ne 2 -or $sessionWindows.Count -ne 2 -or
        $visibleWindows.Count -ne 1 -or $hiddenWindows.Count -ne 1 -or
        $tabState.Selected -lt 0) {
        throw "Expected two hosted tabs with one visible; found tabs=$($tabState.Count), windows=$($sessionWindows.Count), visible=$($visibleWindows.Count), selected=$($tabState.Selected)."
    }
    $sessionHandle = [IntPtr]([int64]$visibleWindows[0])
    $backgroundSessionHandle = [IntPtr]([int64]$hiddenWindows[0])
    $captureHandle = $tabState.Host
    $foregroundHandle = $tabState.Host
} else {
    $sessionHandles = @([NativeFlutterInputUi]::FindVisibleProcessWindows($ProcessId) |
        Where-Object { [IntPtr]([int64]$_) -ne $homeHandle })
    if ($sessionHandles.Count -ne 1) {
        throw "Expected one visible Flutter session window; found $($sessionHandles.Count)."
    }
    $sessionHandle = [IntPtr]([int64]$sessionHandles[0])
    $captureHandle = $sessionHandle
    $foregroundHandle = $sessionHandle
}
$root = [Windows.Automation.AutomationElement]::FromHandle($sessionHandle)
$condition = New-Object Windows.Automation.PropertyCondition(
    [Windows.Automation.AutomationElement]::ClassNameProperty, 'FLUTTERVIEW')
$view = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
if ($null -eq $view -or $view.Current.NativeWindowHandle -eq 0) {
    throw 'The Flutter session render window was not found.'
}
$viewHandle = [IntPtr]$view.Current.NativeWindowHandle
$client = New-Object NativeFlutterInputUi+Rect
if (-not [NativeFlutterInputUi]::GetClientRect($viewHandle, [ref]$client)) {
    throw 'Could not read the Flutter session render bounds.'
}
$width = $client.Right - $client.Left
$height = $client.Bottom - $client.Top
if ($width -lt 800 -or $height -lt 500) {
    throw "Flutter session is too small for input acceptance: ${width}x${height}."
}

$originalCursor = New-Object NativeFlutterInputUi+Point
if (-not [NativeFlutterInputUi]::GetCursorPos([ref]$originalCursor)) {
    throw 'Could not read the original cursor position.'
}
$mouseDown = $false
$keyADown = $false
$controlEnabled = $false
$result = [ordered]@{
    control_on_clicked = $false
    control_on_verified = $false
    control_off_clicked = $false
    control_off_verified = $false
    remote_a_up_before_physical_cleanup = $false
    physical_cleanup_has_no_key_a_event = $false
    input_release_received = $false
    release_held_queued = $false
    release_all_started = $false
    release_all_completed = $false
    injected_a_up_before_physical_cleanup = $false
    control_on_indicator = ''
    control_off_indicator = ''
    tab_switch_released_remote_a = $false
    background_pointer_injection_rejected = $false
    background_session_returned_to_a = $false
    background_session_hold_ms = 0
    background_streaming_before = $false
    background_streaming_after = $false
    background_received_before = 0
    background_received_after = 0
    background_decoded_before = 0
    background_decoded_after = 0
    background_published_before = 0
    background_published_after = 0
    background_recording_before = $false
    background_recording_after = $false
    background_record_samples_before = 0
    background_record_samples_after = 0
    background_recording_path = ''
    session_a_recording_button_verified = $false
    session_a_recording_button_name = ''
    session_a_recording_button_accent_pixels = 0
    session_a_audio_button_name = ''
    session_a_audio_button_click_mode = ''
    session_a_audio_unmute_button_name = ''
    session_a_audio_unmute_click_mode = ''
    session_a_audio_muted_after_toggle = $false
    session_b_audio_unchanged_after_a_toggle = $false
    session_b_audio_muted_after_toggle = $false
    session_a_audio_unchanged_after_b_toggle = $false
    session_a_audio_unmuted_after_toggle = $false
    session_b_audio_state_preserved_after_switch = $false
    audio_isolation_verified = $false
    session_b_recording_button_verified = $false
    session_b_recording_button_name = ''
    session_b_recording_button_accent_pixels = 0
    session_b_audio_button_name = ''
    session_b_audio_button_click_mode = ''
    display_a_window_handle = 0
    display_b_window_handle = 0
    display_a_visible_when_a_selected = $false
    display_b_visible_when_b_selected = $false
    display_windows_follow_parent_tabs = $false
    display_a_closed_after_parent_disconnect = $false
    display_b_survived_parent_disconnect = $false
    sibling_tab_survived_parent_disconnect = $false
    display_window_lifecycle_verified = $false
    shortcut_preference_loaded = $false
    control_enabled_by_shortcut_verified = $false
    stats_shortcut_with_capture_verified = $false
    control_disabled_by_shortcut_verified = $false
    remote_input_blocked_after_control_disabled = $false
    stats_shortcut_after_control_disabled_verified = $false
    function_keys_not_forwarded_to_host = $false
    session_handle = $sessionHandle.ToInt64()
    render_handle = $viewHandle.ToInt64()
    width = $width
    height = $height
}
try {
    if ($VerifyDisplayWindowLifecycle) {
        if (-not $MultiSessionTabs -or
            [string]::IsNullOrWhiteSpace($DisplayDisconnectMarkerPath)) {
            throw 'Display-window lifecycle verification requires two session tabs and a disconnect marker path.'
        }
        $state = [NativeFlutterInputUi]::FindSessionTabs($ProcessId)
        if ($state.Count -ne 2 -or
            @([NativeFlutterInputUi]::FindSessionWindows($state.Host)).Count -ne 2) {
            throw 'Display-window lifecycle verification requires exactly two live session tabs.'
        }
        $displayA = Open-SecondaryDisplayWindow 0 1 'flutter-display-menu-a.png'
        $result.display_a_window_handle = $displayA.ToInt64()
        $state = Select-SessionTabIndex 1
        $displayB = Open-SecondaryDisplayWindow 1 2 'flutter-display-menu-b.png'
        $result.display_b_window_handle = $displayB.ToInt64()

        [void](Select-SessionTabIndex 0)
        Assert-OnlyDisplayVisible $displayA $displayB
        $result.display_a_visible_when_a_selected = $true
        [void](Select-SessionTabIndex 1)
        Assert-OnlyDisplayVisible $displayB $displayA
        $result.display_b_visible_when_b_selected = $true
        $result.display_windows_follow_parent_tabs = $true
        Capture-Window $captureHandle (Join-Path $ArtifactDirectory 'flutter-display-windows-before-disconnect.png')

        Set-Content -LiteralPath $DisplayDisconnectMarkerPath -Value '' -NoNewline
        $deadline = [DateTime]::UtcNow.AddSeconds(25)
        while ([NativeFlutterInputUi]::IsWindow($displayA) -and
            [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
        }
        $result.display_a_closed_after_parent_disconnect =
            -not [NativeFlutterInputUi]::IsWindow($displayA)
        $result.display_b_survived_parent_disconnect =
            [NativeFlutterInputUi]::IsWindow($displayB) -and
            [NativeFlutterInputUi]::IsWindowVisible($displayB)
        $state = [NativeFlutterInputUi]::FindSessionTabs($ProcessId)
        $remainingSessions = @([NativeFlutterInputUi]::FindSessionWindows($state.Host))
        $visibleSessions = @($remainingSessions | Where-Object {
            [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_))
        })
        $result.sibling_tab_survived_parent_disconnect =
            $state.Count -eq 2 -and $state.Selected -eq 1 -and
            $remainingSessions.Count -eq 2 -and $visibleSessions.Count -eq 1
        if (-not $result.display_a_closed_after_parent_disconnect -or
            -not $result.display_b_survived_parent_disconnect -or
            -not $result.sibling_tab_survived_parent_disconnect) {
            throw "Parent disconnect lifecycle failed: A-closed=$($result.display_a_closed_after_parent_disconnect), B-survived=$($result.display_b_survived_parent_disconnect), sibling=$($result.sibling_tab_survived_parent_disconnect)."
        }
        $result.display_window_lifecycle_verified = $true
        Capture-Window $captureHandle (Join-Path $ArtifactDirectory 'flutter-display-windows-after-disconnect.png')
        Write-Output 'Verified display windows follow their parent tabs; disconnecting A closed only A display window while B stayed visible.'
        return
    }
    if ($VerifyKeyboardShortcuts) {
        if ($MultiSessionTabs -or $VerifyAudioIsolation -or
            [string]::IsNullOrWhiteSpace($PreferencesPath) -or
            [string]::IsNullOrWhiteSpace($PeerPreferencesKey)) {
            throw 'Keyboard shortcut verification requires one session and its isolated peer preferences.'
        }
        if ((Get-StatsPreference) -ne $false) {
            throw 'The test peer stats preference must start disabled.'
        }
        $result.shortcut_preference_loaded = $true
        if (-not [NativeFlutterInputUi]::BringToForeground($foregroundHandle)) {
            throw 'Could not focus the Flutter session before the shortcut check.'
        }

        Send-Shortcut 0x76 0x41
        Start-Sleep -Milliseconds 500
        $traceBeforeControl = @(Read-TraceEvents $TracePath)
        $aDownBefore = @($traceBeforeControl | Where-Object {
            $_.kind -eq 'key' -and $_.hid_usage -eq 0x04 -and $_.action -eq 'down'
        }).Count
        $aUpBefore = @($traceBeforeControl | Where-Object {
            $_.kind -eq 'key' -and $_.hid_usage -eq 0x04 -and $_.action -eq 'up'
        }).Count
        Send-TestA $true
        try {
            [void](Wait-Trace { param($items) @($items | Where-Object {
                $_.kind -eq 'key' -and $_.hid_usage -eq 0x04 -and $_.action -eq 'down'
            }).Count -gt $aDownBefore } 8 'A down after Ctrl+F7 enabled control')
        } finally {
            Send-TestA $false
        }
        [void](Wait-Trace { param($items) @($items | Where-Object {
            $_.kind -eq 'key' -and $_.hid_usage -eq 0x04 -and $_.action -eq 'up'
        }).Count -gt $aUpBefore } 8 'A up after control verification')
        $controlEnabled = $true
        $result.control_enabled_by_shortcut_verified = $true

        if (-not [NativeFlutterInputUi]::BringToForeground($foregroundHandle)) {
            throw 'Could not refocus the Flutter session for the captured shortcut check.'
        }
        Start-Sleep -Milliseconds 300
        Send-Shortcut 0x77 0x42
        Wait-StatsPreference $true
        $result.stats_shortcut_with_capture_verified = $true

        if (-not [NativeFlutterInputUi]::BringToForeground($foregroundHandle)) {
            throw 'Could not refocus the Flutter session before disabling control.'
        }
        $releaseBefore = @((Read-TraceEvents $TracePath) | Where-Object {
            $_.kind -eq 'release_control' -and $_.stage -eq 'input_release_received'
        }).Count
        Send-Shortcut 0x76 0x41
        [void](Wait-Trace { param($items) @($items | Where-Object {
            $_.kind -eq 'release_control' -and $_.stage -eq 'input_release_received'
        }).Count -gt $releaseBefore } 8 'InputRelease after Ctrl+F7 disabled control')
        $controlEnabled = $false
        $result.control_disabled_by_shortcut_verified = $true

        if (-not [NativeFlutterInputUi]::BringToForeground($foregroundHandle)) {
            throw 'Could not refocus the Flutter session after disabling control.'
        }
        $traceBeforeDisabledInput = @(Read-TraceEvents $TracePath)
        $disabledAEventsBefore = @($traceBeforeDisabledInput | Where-Object {
            $_.kind -eq 'key' -and $_.hid_usage -eq 0x04
        }).Count
        Send-TestA $true
        Start-Sleep -Milliseconds 100
        Send-TestA $false
        Start-Sleep -Milliseconds 500
        $traceAfterDisabledInput = @(Read-TraceEvents $TracePath)
        $disabledAEventsAfter = @($traceAfterDisabledInput | Where-Object {
            $_.kind -eq 'key' -and $_.hid_usage -eq 0x04
        }).Count
        $result.remote_input_blocked_after_control_disabled =
            $disabledAEventsAfter -eq $disabledAEventsBefore
        if (-not $result.remote_input_blocked_after_control_disabled) {
            throw "Unbound A reached the host after Ctrl+F7 disabled control ($disabledAEventsBefore -> $disabledAEventsAfter)."
        }

        $viewRect = New-Object NativeFlutterInputUi+Rect
        if (-not [NativeFlutterInputUi]::GetWindowRect($viewHandle, [ref]$viewRect)) {
            throw 'Could not read the viewer bounds before checking local shortcut focus.'
        }
        Send-PhysicalClick ($viewRect.Left + [int]($width / 2)) `
            ($viewRect.Top + [int]($height / 2))
        Start-Sleep -Milliseconds 250
        Send-Shortcut 0x77 0x42
        Wait-StatsPreference $false
        $result.stats_shortcut_after_control_disabled_verified = $true
        $trace = @(Read-TraceEvents $TracePath)
        $result.function_keys_not_forwarded_to_host = @($trace | Where-Object {
            $_.kind -eq 'key' -and ($_.hid_usage -eq 0x40 -or $_.hid_usage -eq 0x41)
        }).Count -eq 0
        if (-not $result.function_keys_not_forwarded_to_host) {
            throw 'A configured Ctrl+F7/F8 key reached the loopback host.'
        }
        Capture-Window $captureHandle (Join-Path $ArtifactDirectory 'flutter-shortcut-stats.png')
        Write-Output 'Verified Ctrl+F7 toggled remote control, Ctrl+F8 toggled only local stats, and remote input stopped after control was disabled.'
        return
    }
    if ($VerifyAudioIsolation) {
        if (-not $MultiSessionTabs -or
            [string]::IsNullOrWhiteSpace($AudioStatusPathA) -or
            [string]::IsNullOrWhiteSpace($AudioStatusPathB)) {
            throw 'Audio isolation verification requires two session tabs and both status paths.'
        }
        $beforeA = Read-AudioStatus $AudioStatusPathA
        $beforeB = Read-AudioStatus $AudioStatusPathB
        if ($null -eq $beforeA -or $null -eq $beforeB -or
            $beforeA.State -ne 3 -or $beforeB.State -ne 3 -or
            $beforeA.AudioEnabled -ne 1 -or $beforeB.AudioEnabled -ne 1 -or
            $beforeA.AudioMuted -ne 0 -or $beforeB.AudioMuted -ne 0 -or
            $beforeA.AudioDecoded -lt 30 -or $beforeB.AudioDecoded -lt 30) {
            throw "Both sessions must be streaming, decoding synthetic audio and initially unmuted: A=$beforeA B=$beforeB."
        }
        if (-not [NativeFlutterInputUi]::BringToForeground($captureHandle)) {
            throw 'Could not bring the isolated Flutter tab host to the foreground.'
        }

        $tabState = Select-SessionTabIndex 0
        $visibleA = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host) |
            Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
        if ($tabState.Selected -ne 0 -or $visibleA.Count -ne 1) {
            throw 'Could not select the first audio test session.'
        }
        $sessionA = [IntPtr]([int64]$visibleA[0])
        $viewA = [IntPtr]([NativeFlutterInputUi]::FindFlutterViewWindow($sessionA))
        $clientA = New-Object NativeFlutterInputUi+Rect
        if ($viewA -eq [IntPtr]::Zero -or
            -not [NativeFlutterInputUi]::GetClientRect($viewA, [ref]$clientA)) {
            throw 'Could not find the first audio session render view.'
        }
        $widthA = $clientA.Right - $clientA.Left
        Show-Toolbar $captureHandle $viewA $widthA $ArtifactDirectory
        $audioButtonA = Invoke-AudioToggle $captureHandle $viewA $widthA 'flutter-tab-a-audio-before.png'
        $result.session_a_audio_button_name = $audioButtonA.Name
        $result.session_a_audio_button_click_mode = $audioButtonA.ClickMode
        $mutedA = Wait-AudioStatus $AudioStatusPathA 1 $beforeA.AudioDecoded
        $unchangedB = Wait-AudioStatus $AudioStatusPathB 0 $beforeB.AudioDecoded
        $result.session_a_audio_muted_after_toggle = $mutedA.AudioMuted -eq 1
        $result.session_b_audio_unchanged_after_a_toggle = $unchangedB.AudioMuted -eq 0
        $result.session_a_audio_decoded_after_mute = $mutedA.AudioDecoded
        $result.session_b_audio_decoded_after_a_toggle = $unchangedB.AudioDecoded
        Capture-Window $captureHandle (Join-Path $ArtifactDirectory 'flutter-tab-a-audio-muted.png')

        $tabState = Select-SessionTabIndex 1
        $visibleB = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host) |
            Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
        if ($tabState.Selected -ne 1 -or $visibleB.Count -ne 1) {
            throw 'Could not select the second audio test session.'
        }
        $sessionB = [IntPtr]([int64]$visibleB[0])
        $viewB = [IntPtr]([NativeFlutterInputUi]::FindFlutterViewWindow($sessionB))
        $clientB = New-Object NativeFlutterInputUi+Rect
        if ($viewB -eq [IntPtr]::Zero -or
            -not [NativeFlutterInputUi]::GetClientRect($viewB, [ref]$clientB)) {
            throw 'Could not find the second audio session render view.'
        }
        $widthB = $clientB.Right - $clientB.Left
        Show-Toolbar $captureHandle $viewB $widthB $ArtifactDirectory
        $audioButtonB = Invoke-AudioToggle $captureHandle $viewB $widthB 'flutter-tab-b-audio-before.png'
        $result.session_b_audio_button_name = $audioButtonB.Name
        $result.session_b_audio_button_click_mode = $audioButtonB.ClickMode
        $mutedB = Wait-AudioStatus $AudioStatusPathB 1 $beforeB.AudioDecoded
        $stillMutedA = Wait-AudioStatus $AudioStatusPathA 1 $mutedA.AudioDecoded
        $result.session_b_audio_muted_after_toggle = $mutedB.AudioMuted -eq 1
        $result.session_a_audio_unchanged_after_b_toggle = $stillMutedA.AudioMuted -eq 1
        $result.session_a_audio_decoded_after_b_toggle = $stillMutedA.AudioDecoded
        $result.session_b_audio_decoded_after_mute = $mutedB.AudioDecoded
        Capture-Window $captureHandle (Join-Path $ArtifactDirectory 'flutter-tab-b-audio-muted.png')

        $tabState = Select-SessionTabIndex 0
        $visibleA = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host) |
            Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
        if ($tabState.Selected -ne 0 -or $visibleA.Count -ne 1) {
            throw 'Could not return to the first audio session.'
        }
        $viewA = [IntPtr]([NativeFlutterInputUi]::FindFlutterViewWindow(
            [IntPtr]([int64]$visibleA[0])))
        $clientA = New-Object NativeFlutterInputUi+Rect
        if ($viewA -eq [IntPtr]::Zero -or
            -not [NativeFlutterInputUi]::GetClientRect($viewA, [ref]$clientA)) {
            throw 'Could not find the first audio session after switching back.'
        }
        $widthA = $clientA.Right - $clientA.Left
        Show-Toolbar $captureHandle $viewA $widthA $ArtifactDirectory
        $audioButtonA = Invoke-AudioToggle $captureHandle $viewA $widthA 'flutter-tab-a-audio-unmute-before.png'
        $result.session_a_audio_unmute_button_name = $audioButtonA.Name
        $result.session_a_audio_unmute_click_mode = $audioButtonA.ClickMode
        $unmutedA = Wait-AudioStatus $AudioStatusPathA 0 $stillMutedA.AudioDecoded
        $preservedB = Wait-AudioStatus $AudioStatusPathB 1 $mutedB.AudioDecoded
        $result.session_a_audio_unmuted_after_toggle = $unmutedA.AudioMuted -eq 0
        $result.session_b_audio_state_preserved_after_switch = $preservedB.AudioMuted -eq 1
        $result.session_a_audio_decoded_after_unmute = $unmutedA.AudioDecoded
        $result.session_b_audio_decoded_after_switch = $preservedB.AudioDecoded
        $result.audio_isolation_verified = $true
        Capture-Window $captureHandle (Join-Path $ArtifactDirectory 'flutter-tab-a-audio-unmuted.png')
        Write-Output 'Verified Flutter tab audio routing: mute A leaves B unmuted; mute B leaves A muted; unmute A leaves B muted.'
        return
    }
    $windowRect = New-Object NativeFlutterInputUi+Rect
    if (-not [NativeFlutterInputUi]::GetWindowRect($viewHandle, [ref]$windowRect)) {
        throw 'Could not read the Flutter render window screen position.'
    }
    $screen = New-Object NativeFlutterInputUi+Point
    $screen.X = $windowRect.Left + [int]($width / 2)
    $screen.Y = $windowRect.Top + [int]($height / 2)

    $foreground = [NativeFlutterInputUi]::GetForegroundWindow()
    $foregroundRoot = [NativeFlutterInputUi]::GetAncestor($foreground, 2)
    if ($foreground -ne $foregroundHandle -and $foregroundRoot -ne $foregroundHandle) {
        if (-not [NativeFlutterInputUi]::BringToForeground($foregroundHandle)) {
            throw "Could not bring the Flutter session window to the foreground (foreground=$foreground foreground window=$foregroundHandle)."
        }
        Start-Sleep -Milliseconds 300
        $foreground = [NativeFlutterInputUi]::GetForegroundWindow()
        $foregroundRoot = [NativeFlutterInputUi]::GetAncestor($foreground, 2)
        if ($foreground -ne $foregroundHandle -and $foregroundRoot -ne $foregroundHandle) {
            throw "The Flutter session window did not become foreground (foreground=$foreground foreground window=$foregroundHandle)."
        }
    }

    Capture-Window $captureHandle (Join-Path $ArtifactDirectory 'flutter-input-before.png')
    Show-Toolbar $captureHandle $viewHandle $width $ArtifactDirectory
    $keyboardButton = Find-ToolbarKeyboardButton $captureHandle $viewHandle $ArtifactDirectory
    if ($keyboardButton.RemoteControlEnabled) {
        throw "Remote input control was already enabled before acceptance: '$($keyboardButton.Name)'."
    }
    $keyboardX = $keyboardButton.ClientX
    $keyboardY = $keyboardButton.ClientY
    $result.keyboard_button_x = $keyboardX
    $result.keyboard_button_y = $keyboardY
    $controlEnabled = $true
    Invoke-ToolbarKeyboardButton $keyboardButton $viewHandle
    $result.control_on_clicked = $true
    $controlOnButton = Wait-ToolbarRemoteControlState $viewHandle $true
    $result.control_on_indicator = $controlOnButton.Name
    $controlOnScreenshot = Join-Path $ArtifactDirectory 'flutter-input-control-on.png'
    Capture-Window $captureHandle $controlOnScreenshot

    if (-not $MultiSessionTabs) {
        [void][NativeFlutterInputUi]::SetCursorPos($screen.X, $screen.Y)
        Start-Sleep -Milliseconds 200
        [NativeFlutterInputUi]::MouseButton(0x0002)
        $mouseDown = $true
        [NativeFlutterInputUi]::MouseButton(0x0004)
        $mouseDown = $false
        $events = @(Wait-Trace { param($items) @($items | Where-Object { $_.kind -eq 'mouse' }).Count -gt 0 } 10 'mouse event')
        $result.mouse_events = @($events | Where-Object { $_.kind -eq 'mouse' }).Count
    }

    [NativeFlutterInputUi]::Key(0x41, 0x1E, 0)
    $keyADown = $true
    $events = @(Wait-Trace { param($items) @($items | Where-Object { $_.kind -eq 'key' -and $_.hid_usage -eq 4 -and $_.action -eq 'down' }).Count -gt 0 } 10 'A down')
    $result.a_down_events = @($events | Where-Object { $_.kind -eq 'key' -and $_.hid_usage -eq 4 -and $_.action -eq 'down' }).Count
    $result.control_on_verified = $true
    Start-Sleep -Milliseconds 400

    if ($MultiSessionTabs) {
        $traceBeforeSwitch = @(Read-TraceEvents $TracePath)
        $releaseReceivedBefore = @($traceBeforeSwitch | Where-Object {
            $_.kind -eq 'release_control' -and $_.stage -eq 'input_release_received'
        }).Count
        $releaseQueuedBefore = @($traceBeforeSwitch | Where-Object {
            $_.kind -eq 'release_control' -and $_.stage -eq 'release_held_queued'
        }).Count
        $releaseAllBefore = @($traceBeforeSwitch | Where-Object {
            $_.kind -eq 'release_control' -and $_.stage -eq 'release_all_begin'
        }).Count

        $otherIndex = ($tabState.Selected + 1) % $tabState.Count
        $tabRect = New-Object NativeFlutterInputUi+Rect
        if (-not [NativeFlutterInputUi]::GetTabItemRect($tabState.Tabs, $otherIndex, [ref]$tabRect)) {
            throw "Could not read tab $otherIndex bounds."
        }
        Send-Click $tabState.Tabs ([int](($tabRect.Left + $tabRect.Right) / 2)) `
            ([int](($tabRect.Top + $tabRect.Bottom) / 2))

        $switchDeadline = [DateTime]::UtcNow.AddSeconds(5)
        do {
            Start-Sleep -Milliseconds 50
            $tabState = [NativeFlutterInputUi]::FindSessionTabs($ProcessId)
            $trace = @(Read-TraceEvents $TracePath)
            $releaseSequence = Get-AReleaseSequence $trace `
                $releaseReceivedBefore $releaseQueuedBefore $releaseAllBefore
            $visibleWindows = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host) |
                Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
        } while (($tabState.Selected -ne $otherIndex -or $visibleWindows.Count -ne 1 -or
            -not $releaseSequence.Received -or -not $releaseSequence.Queued -or
            -not $releaseSequence.ReleaseAllStarted -or
            -not $releaseSequence.ReleaseAllCompleted -or
            -not $releaseSequence.InjectedAUp) -and [DateTime]::UtcNow -lt $switchDeadline)

        if ($tabState.Selected -ne $otherIndex -or $visibleWindows.Count -ne 1 -or
            -not $releaseSequence.Received -or -not $releaseSequence.Queued -or
            -not $releaseSequence.ReleaseAllStarted -or
            -not $releaseSequence.ReleaseAllCompleted -or
            -not $releaseSequence.InjectedAUp) {
            throw "Switching away did not release remote A: selected=$($tabState.Selected), visible=$($visibleWindows.Count), received=$($releaseSequence.Received), queued=$($releaseSequence.Queued), started=$($releaseSequence.ReleaseAllStarted), completed=$($releaseSequence.ReleaseAllCompleted), A-up=$($releaseSequence.InjectedAUp)."
        }
        if ([IntPtr]([int64]$visibleWindows[0]) -ne $backgroundSessionHandle) {
            throw "Tab selection changed to $otherIndex, but the expected sibling window $backgroundSessionHandle is not visible."
        }
        $result.tab_switch_released_remote_a = $true
        $result.input_release_received = $releaseSequence.Received
        $result.release_held_queued = $releaseSequence.Queued
        $result.release_all_started = $releaseSequence.ReleaseAllStarted
        $result.release_all_completed = $releaseSequence.ReleaseAllCompleted
        $result.injected_a_up_before_physical_cleanup = $releaseSequence.InjectedAUp
        $result.trace_events_before_physical_cleanup = $trace.Count
        $result.remote_a_up_before_physical_cleanup = $true
        $controlEnabled = $false

        $backgroundViewHandle = [IntPtr]([NativeFlutterInputUi]::FindFlutterViewWindow($sessionHandle))
        if ($backgroundViewHandle -eq [IntPtr]::Zero -or
            -not [NativeFlutterInputUi]::IsWindow($backgroundViewHandle)) {
            throw 'The now-hidden Flutter render window was not found.'
        }
        if ([NativeFlutterInputUi]::IsWindowVisible($backgroundViewHandle)) {
            throw 'The prior session render window remained visible after switching tabs.'
        }
        $backgroundClient = New-Object NativeFlutterInputUi+Rect
        if (-not [NativeFlutterInputUi]::GetClientRect($backgroundViewHandle, [ref]$backgroundClient)) {
            throw 'Could not read the hidden Flutter render bounds.'
        }
        $mouseEventsBefore = @($trace | Where-Object { $_.kind -eq 'mouse' }).Count
        $result.trace_events_before_background_input = $trace.Count
        Send-Click $backgroundViewHandle `
            ([int](($backgroundClient.Right - $backgroundClient.Left) / 2)) `
            ([int](($backgroundClient.Bottom - $backgroundClient.Top) / 2))
        Start-Sleep -Milliseconds 800
        $afterBackgroundInput = @(Read-TraceEvents $TracePath)
        $mouseEventsAfter = @($afterBackgroundInput | Where-Object { $_.kind -eq 'mouse' }).Count
        $result.background_pointer_injection_rejected = $mouseEventsAfter -eq $mouseEventsBefore
        $result.background_view_handle = $backgroundViewHandle.ToInt64()
        $result.background_mouse_events_before = $mouseEventsBefore
        $result.background_mouse_events_after = $mouseEventsAfter
        if (-not $result.background_pointer_injection_rejected) {
            throw "A pointer click sent to the hidden session reached the host: mouse events $mouseEventsBefore -> $mouseEventsAfter."
        }
        $result.trace_events_after_background_input = $afterBackgroundInput.Count

        if ($HoldBackgroundSession) {
            $statusBefore = Wait-BackgroundStatus $BackgroundStatusPath
            $result.background_streaming_before = $statusBefore.State -eq 3
            $result.background_received_before = $statusBefore.Received
            $result.background_decoded_before = $statusBefore.Decoded
            $result.background_published_before = $statusBefore.Published
            $result.background_recording_before = $statusBefore.Recording -eq 1
            $result.background_record_samples_before = $statusBefore.RecordSamples
            $result.background_recording_path = $statusBefore.RecordPath

            $tabState = Select-SessionTabIndex 0
            $visibleAfterReturn = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host) |
                Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
            if ($tabState.Selected -ne 0 -or $visibleAfterReturn.Count -ne 1 -or
                [IntPtr]([int64]$visibleAfterReturn[0]) -ne $sessionHandle) {
                throw 'Could not return to session A while keeping recording session B hidden.'
            }
            $result.background_session_returned_to_a = $true
            $holdWatch = [Diagnostics.Stopwatch]::StartNew()
            $holdDeadline = [DateTime]::UtcNow.AddSeconds(8)
            $statusAfter = $null
            do {
                $candidate = Read-BackgroundStatus $BackgroundStatusPath
                if ($null -ne $candidate -and $holdWatch.ElapsedMilliseconds -ge 1800 -and
                    $candidate.State -eq 3 -and $candidate.Recording -eq 1 -and
                    $candidate.Received -gt $statusBefore.Received -and
                    $candidate.Decoded -gt $statusBefore.Decoded -and
                    $candidate.Published -gt $statusBefore.Published -and
                    $candidate.RecordSamples -gt $statusBefore.RecordSamples) {
                    $statusAfter = $candidate
                    break
                }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $holdDeadline)
            $holdWatch.Stop()
            if ($null -eq $statusAfter) {
                $last = Read-BackgroundStatus $BackgroundStatusPath
                throw "Hidden session did not remain streaming/recording and advance all counters; baseline=$($statusBefore | ConvertTo-Json -Compress), latest=$($last | ConvertTo-Json -Compress)."
            }
            $result.background_session_hold_ms = [int]$holdWatch.ElapsedMilliseconds
            $result.background_streaming_after = $statusAfter.State -eq 3
            $result.background_received_after = $statusAfter.Received
            $result.background_decoded_after = $statusAfter.Decoded
            $result.background_published_after = $statusAfter.Published
            $result.background_recording_after = $statusAfter.Recording -eq 1
            $result.background_record_samples_after = $statusAfter.RecordSamples

            Show-Toolbar $captureHandle $viewHandle $width $ArtifactDirectory
            $sessionAScreenshot = Join-Path $ArtifactDirectory 'flutter-tab-a-recording-button.png'
            Capture-Window $captureHandle $sessionAScreenshot
            $sessionAAccentPixels = Get-RecordingToolbarAccentPixels `
                $sessionAScreenshot $captureHandle $viewHandle
            $result.session_a_recording_button_accent_pixels = $sessionAAccentPixels
            $result.session_a_recording_button_verified = $sessionAAccentPixels -le 4
            $result.session_a_recording_button_name = "visual inactive (accent pixels=$sessionAAccentPixels)"
            if (-not $result.session_a_recording_button_verified) {
                throw "Session A recording control appeared active in its screenshot ($sessionAAccentPixels accent pixels)."
            }
            $result.session_a_recording_screenshot = $sessionAScreenshot

            $tabState = Select-SessionTabIndex 1
            $visibleForB = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host) |
                Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
            if ($tabState.Selected -ne 1 -or $visibleForB.Count -ne 1 -or
                [IntPtr]([int64]$visibleForB[0]) -ne $backgroundSessionHandle) {
                throw 'Could not select recording session B for the toolbar state check.'
            }
            $viewB = [IntPtr]([NativeFlutterInputUi]::FindFlutterViewWindow($backgroundSessionHandle))
            $clientB = New-Object NativeFlutterInputUi+Rect
            if ($viewB -eq [IntPtr]::Zero -or
                -not [NativeFlutterInputUi]::GetClientRect($viewB, [ref]$clientB)) {
                throw 'Could not read recording session B render bounds.'
            }
            $widthB = $clientB.Right - $clientB.Left
            Show-Toolbar $captureHandle $viewB $widthB $ArtifactDirectory
            $sessionBScreenshot = Join-Path $ArtifactDirectory 'flutter-tab-b-recording-button.png'
            Capture-Window $captureHandle $sessionBScreenshot
            $sessionBAccentPixels = Get-RecordingToolbarAccentPixels `
                $sessionBScreenshot $captureHandle $viewB
            $result.session_b_recording_button_accent_pixels = $sessionBAccentPixels
            $result.session_b_recording_button_verified = $sessionBAccentPixels -ge 20
            $result.session_b_recording_button_name = "visual active (accent pixels=$sessionBAccentPixels)"
            if (-not $result.session_b_recording_button_verified) {
                throw "Session B recording control did not appear active in its screenshot ($sessionBAccentPixels accent pixels)."
            }
            $result.session_b_recording_screenshot = $sessionBScreenshot

            $tabState = Select-SessionTabIndex 0
            $visibleAfterUiCheck = @([NativeFlutterInputUi]::FindSessionWindows($tabState.Host) |
                Where-Object { [NativeFlutterInputUi]::IsWindowVisible([IntPtr]([int64]$_)) })
            if ($tabState.Selected -ne 0 -or $visibleAfterUiCheck.Count -ne 1 -or
                [IntPtr]([int64]$visibleAfterUiCheck[0]) -ne $sessionHandle) {
                throw 'Could not restore session A after recording toolbar state checks.'
            }
            Write-Output "Verified per-tab recording toolbar pixels: A=$sessionAAccentPixels (inactive), B=$sessionBAccentPixels (active); UI Automation is unavailable for Flutter controls."
        }
        Write-Output "Verified tab switch released remote A and hidden-view pointer input was rejected (A=$sessionHandle, hidden-view=$backgroundViewHandle, selected=$otherIndex)."
    } else {

    $traceBeforeControlOff = @(Read-TraceEvents $TracePath)
    $releaseReceivedBefore = @($traceBeforeControlOff | Where-Object {
        $_.kind -eq 'release_control' -and $_.stage -eq 'input_release_received'
    }).Count
    $releaseQueuedBefore = @($traceBeforeControlOff | Where-Object {
        $_.kind -eq 'release_control' -and $_.stage -eq 'release_held_queued'
    }).Count
    $releaseAllBefore = @($traceBeforeControlOff | Where-Object {
        $_.kind -eq 'release_control' -and $_.stage -eq 'release_all_begin'
    }).Count

    Show-Toolbar $captureHandle $viewHandle $width $ArtifactDirectory
    $activeButton = Wait-ToolbarRemoteControlState $viewHandle $true
    Invoke-ToolbarKeyboardButton $activeButton $viewHandle
    $result.control_off_clicked = $true
    $controlOffButton = Wait-ToolbarRemoteControlState $viewHandle $false
    $result.control_off_indicator = $controlOffButton.Name
    $controlOffScreenshot = Join-Path $ArtifactDirectory 'flutter-input-control-off.png'
    Capture-Window $captureHandle $controlOffScreenshot

    $releaseDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $events = @(Read-TraceEvents $TracePath)
        $releaseSequence = Get-AReleaseSequence $events `
            $releaseReceivedBefore $releaseQueuedBefore $releaseAllBefore
        $result.input_release_received = $releaseSequence.Received
        $result.release_held_queued = $releaseSequence.Queued
        $result.release_all_started = $releaseSequence.ReleaseAllStarted
        $result.release_all_completed = $releaseSequence.ReleaseAllCompleted
        $result.injected_a_up_before_physical_cleanup = $releaseSequence.InjectedAUp
        if ($result.input_release_received -and $result.release_held_queued -and
            $result.release_all_started -and $result.release_all_completed -and
            $result.injected_a_up_before_physical_cleanup) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $releaseDeadline)
    if (-not $result.input_release_received -or -not $result.release_held_queued -or
        -not $result.release_all_started -or -not $result.release_all_completed -or
        -not $result.injected_a_up_before_physical_cleanup) {
        throw "Timed out waiting for InputRelease/release_held/release_all A-up evidence: received=$($result.input_release_received), queued=$($result.release_held_queued), started=$($result.release_all_started), completed=$($result.release_all_completed), A-up=$($result.injected_a_up_before_physical_cleanup)."
    }
    $controlEnabled = $false
    $result.control_off_verified = $true
    $beforeCleanupCount = $events.Count
    $result.remote_a_up_before_physical_cleanup = $true
    $result.trace_events_before_physical_cleanup = $beforeCleanupCount

    Start-Sleep -Milliseconds 900
    [NativeFlutterInputUi]::Key(0x41, 0x1E, 0x0002)
    $keyADown = $false
    Start-Sleep -Milliseconds 1000
    $afterEvents = @(Read-TraceEvents $TracePath)
    if ($afterEvents.Count -le $beforeCleanupCount) {
        $newAEvents = @()
    } else {
        $newAEvents = @($afterEvents[$beforeCleanupCount..($afterEvents.Count - 1)] |
            Where-Object { $_.kind -eq 'key' -and $_.hid_usage -eq 4 })
    }
    $result.physical_cleanup_has_no_key_a_event = ($newAEvents.Count -eq 0)
    $result.trace_events_after_physical_cleanup = $afterEvents.Count
    if (-not $result.physical_cleanup_has_no_key_a_event) {
        throw "Physical A cleanup unexpectedly produced $($newAEvents.Count) new A events."
    }
    }
} finally {
    if ($controlEnabled) {
        $result.cleanup_control_off_attempted = $true
        try {
            if ($VerifyKeyboardShortcuts) {
                if ([NativeFlutterInputUi]::BringToForeground($foregroundHandle)) {
                    Send-Shortcut 0x76 0x41
                    $controlEnabled = $false
                    $result.cleanup_control_off_clicked = $true
                }
            } else {
                Show-Toolbar $captureHandle $viewHandle $width $ArtifactDirectory
                $cleanupButton = Find-ToolbarKeyboardButton $captureHandle $viewHandle $ArtifactDirectory
                if ($cleanupButton.RemoteControlEnabled) {
                    Invoke-ToolbarKeyboardButton $cleanupButton $viewHandle
                    [void](Wait-ToolbarRemoteControlState $viewHandle $false)
                    $result.cleanup_control_off_clicked = $true
                }
                $controlEnabled = $false
            }
        } catch {
            $result.cleanup_control_off_error = $_.Exception.Message
        }
    }
    if ($keyADown) {
        try { [NativeFlutterInputUi]::Key(0x41, 0x1E, 0x0002) } catch {}
    }
    if ($mouseDown) {
        try { [NativeFlutterInputUi]::MouseButton(0x0004) } catch {}
    }
    if ($originalCursor -ne $null) {
        try { [void][NativeFlutterInputUi]::SetCursorPos($originalCursor.X, $originalCursor.Y) } catch {}
    }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ArtifactDirectory 'flutter-input-results.json') -Encoding UTF8
}
Write-Output "Flutter input acceptance complete: $ArtifactDirectory"
