# 输入通道开发记录

本文记录 chast 远程控制输入通道的设计与实现进度，按阶段（P1/P2/P3）更新。

## 目标与分工

- 协议层与平台无关：坐标用**远端虚拟桌面像素**，按键用 **USB HID usage**。
- 被控端（服务端）负责把事件真正注入本机；客户端只负责采集、映射、发送。
- 输入默认关闭，必须由服务端显式开启，并在 HelloAck 中告知客户端。

## P1 — 协议 + 服务端注入（已完成）

### 协议（`chast/protocol.h`，版本 2）

控制流新增 `ControlType::InputEvent = 6`，载荷固定 16 字节（P3 起末字段为输入状态位图）：

| 字段 | 类型 | 说明 |
|---|---|---|
| kind | u8 | 1 = 鼠标，2 = 键盘 |
| action | u8 | 鼠标：Move/MoveRelative/ButtonDown/ButtonUp/Wheel/WheelHorizontal；键盘：Down/Up |
| buttons | u16 | 鼠标按键位（左 1、右 2、中 4、后 8、前 16）；键盘为 0 |
| x | i32 | 绝对事件为虚拟桌面像素；相对移动为增量；横向滚轮为 WHEEL_DELTA 单位 |
| y | i32 | 同上；纵向滚轮用 y |
| hid_usage | u16 | 键盘专用，usage page 0x07 |
| 输入状态 | u16 | 修饰键位图 + 锁键状态（P3） |

`HelloAck` 增加：

- `origin_x` / `origin_y`：被采集输出在虚拟桌面中的左上角（可为负），客户端把画布坐标换算成帧像素后加上它。
- `flags` bit0：`kServerInfoInputEnabled`，服务端是否接受输入。
- 版本号提升到 2；ALPN 仍为 `chast/1`（家族名，版本不匹配由 Hello 报出具体数字）。

设计取舍：

- 鼠标和键盘分成两种载荷，而不是像 RustDesk 那样挤进一个 mask；解析失败时错误信息更明确。
- 事件逐条发送，不合并、不批量：QUIC 流有序，注入线程串行应用，语义最简单。
- 坐标不做归一化，直接传虚拟桌面像素；多屏、负坐标天然成立，服务端不需要知道客户端在看哪块屏。

### 服务端注入（`include/chast/input.h`、`src/win/input.cc`）

- `hid_to_scancode()`：HID usage page 0x07 → PS/2 set 1 扫描码表，附带 extended 标志。Pause（需要 E1 前缀）与 F13–F24 明确拒绝。
- `InputInjector`：
  - `submit()` 只入队，不阻塞网络回调线程；队列上限 1024，满了计数丢弃。
  - 单线程串行应用，`SendInput` 只在注入线程调用。
  - 键盘用 `KEYEVENTF_SCANCODE`（与布局无关），扩展键加 `KEYEVENTF_EXTENDEDKEY`。
  - 鼠标绝对定位用 `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK`，按虚拟桌面换算 0..65535；坐标来自对端，先按 i64 缩放再夹取，避免溢出。
  - 记录按下的按键（含 extended 位）与鼠标键；`stop()` 时反序全部释放，断线不会留下卡键。
  - 自动重复产生多次 Down，只在第一次按下时记入 held 集合，避免一次 Up 之后残留。
  - `dry_run` 模式：除 `SendInput` 外整条路径照常执行，供回环测试使用，不移动真实指针。

### 会话层（`include/chast/quic.h`、`src/net/quic.cc`）

- `QuicServerConfig::allow_input`（默认 `false`）与 `dry_run_input`。
- `announce()` 以服务端配置为准写 HelloAck 的 input 标志，调用方无法越过这道闸门。
- 服务端收到 `InputEvent` 时：未开启输入则静默丢弃（HelloAck 已经告知）；载荷畸形则按协议违规结束会话（与 Stats/Hello 一致）。
- 新增 `take_input_events()`（观测/测试用，环形上限 4096）与 `input_stats()`。
- 客户端新增 `send_input()`。
- `Impl::close()` 先停注入线程，再关流/连接。

### 工具与测试

- `chast-quic serve --input`：接受并注入输入。
- `chast-quic connect --input`：握手后回放脚本（移动、点击、双向滚轮、相对移动、Shift+A），真实注入。
- `chast-quic verify`：默认开启输入并以 dry-run 注入，校验服务端收到的事件与客户端发送的**逐条相同且顺序一致**，且全部成功注入、无拒绝/失败/丢弃。
- `chast-protocol-test`（新增，加入 ctest）：
  - HelloAck 往返（含负 origin、输入标志、编码器名）；
  - InputEvent 往返与非法载荷拒绝（未知 kind/action/buttons、缺少 hid_usage）；
  - HID → 扫描码映射抽查；
  - 注入器 dry-run：计数、未知键拒绝、`stop()` 释放未松开的键；
  - `--live-input` 可选：真实移动指针并读回校验（±2 像素）。

### 验证结果

```
ctest -C Release
1/3 cursor        Passed
2/3 protocol      Passed
3/3 quic_loopback Passed   6.0 s
```

回环检查中除原有视频项外新增：

```
ok   the server announced input as enabled
ok   the client saw input enabled in HelloAck
ok   every input event reached the server
ok   input events arrived intact and in order
ok   every input event was injected
ok   no input event was rejected, failed or dropped
```

`--live-input` 实测：目标 (1440,540)，落点 (1439,539)（绝对注入量化误差，符合预期）。

服务端未开 `--input` 时，`connect --input` 在握手后立即报 `the server refused input`，不会发送任何事件。

## P2 — Flutter 端采集与坐标映射（已完成）

### 服务端 origin

- `chast-quic serve` 用 `capturer->display().rect` 的左上角填充 `HelloAck.origin_x/origin_y`，不再固定为 0；副屏、负坐标和多显示器因此成立。
- `verify` 新增断言：客户端看到的 origin 必须等于采集输出的虚拟桌面原点。

### 会话层与原生 bridge

- `ViewerEngine::SendInput()` 只把事件放进上限 256 的队列，不触碰 QUIC；队列满时拒绝新事件。会话线程独占 `QuicSession`，在每次 `receive_frame` 前排空队列并按序 `send_input()`，网络回调与 UI 线程都不直接发送。
- 服务端公布输入开启时，接收轮询从 250 ms 降到 10 ms，静止桌面上的指针延迟不再被视频节奏拖住；输入关闭时保持 250 ms，不增加唤醒。
- `ViewerStatus` 增加 `input_enabled`、`origin_x/origin_y`、`input_sent/input_failed`；握手后同步，连接结束清零。
- MethodChannel 新增 `sendInput`，按 kind 校验动作、按键位、坐标范围、滚轮不带键、按键事件必须带 usage；队列拒绝返回 `false` 而不是异常。

### Flutter 端

- 工具栏键盘按钮成为控制开关：只在服务端开放输入且流已进入 streaming 时可用；断线、结束、重连自动关闭。开启后本地指针在画面区域隐藏，点击画面会重新获取键盘焦点。
- 指针在 `RemoteView` 内换算成**帧像素**（自适应/原始大小两种布局各自处理），会话页再加 origin 并夹取到该输出范围；鼠标按键位直接使用 Flutter 的 `PointerEvent.buttons`（位布局与协议一致）。
- 按键取 `physicalKey.usbHidUsage`，只转发 Down/Up，忽略 `KeyRepeatEvent`（自动重复由被控端生成）。
- 滚轮按 Flutter Windows 的 53 逻辑像素/格累积，换算成 `WHEEL_DELTA`（120）单位；纵向取反以匹配 Windows 的“向上为正”。
- 控制开启时 Esc/F11 也转发给远端，本地退出/全屏改走工具栏；原始大小模式下控制开启时不再本地滚动，滚轮完全归远端。
- 关闭控制（按钮关闭、断线、重连）时按反序补发已按下按键的 Up，并抬起仍按着的鼠标键，滚轮累积器清零，避免被控端留下卡键或残留按键状态。

### 工具

- `chast-quic serve --input-dry-run`：接受输入但停在 `SendInput` 之前，用于在同机运行 Flutter 客户端做端到端演练，注入事件不会回环到客户端窗口。
- `chast-quic serve --dump-input`：打印服务端实际收到的每个 InputEvent，便于核对映射。

### 验证结果

```
ctest -C Release
1/3 cursor        Passed
2/3 protocol      Passed
3/3 quic_loopback Passed   6.1 s
```

回环新增：`ok the viewer saw the captured output's virtual-desktop origin`。

Flutter 客户端与 `serve --input-dry-run` 同机演练：

```
sender: input received=537 applied=537 rejected=0 failed=0 dropped=0
```

覆盖指针移动、左键 Down/Up（由脚本点击画面产生）与视图坐标映射；滚轮单独验证得到 `action=4 y=120` 与 `y=-120`，与物理滚轮方向一致。

最终 release 构建复测（`serve --input-dry-run --dump-input`）：

```
input: mouse action=4 buttons=0 x=0 y=120
input: mouse action=4 buttons=0 x=0 y=-120
input: mouse action=2 buttons=1 x=0 y=0
input: mouse action=3 buttons=1 x=0 y=0
sender: input received=14 applied=14 rejected=0 failed=0 dropped=0
```

关闭控制时补发的释放事件也走同一队列；服务端在客户端断线时另有 held 集合兜底。

未能自动验证的部分：

- 本机自动化环境会拦截合成的键盘事件（独立 WinForms 文本框同样收不到），因此 Flutter 的按键转发没有做端到端回放；HID→扫描码与注入路径由 `protocol` 测试和 CLI 回环的 Shift+A 覆盖，`physicalKey.usbHidUsage` 分支需用真实键盘再确认一次。
- 自动化注入 `WM_MOUSEHWHEEL` 时客户端没有产生横向滚动事件，`WheelHorizontal` 目前只有协议/CLI 回环覆盖。

## P3 — 修饰键、锁定键与卡键保护

### 协议 v3：输入状态位图（已完成）

- `InputEvent` 末尾原来的 reserved u16 改为 `modifiers` 输入状态位图：bit0..7 依次对应 HID usage 0xE0..0xE7（左/右 Ctrl、Shift、Alt、Gui，AltGr 即右 Alt），bit8..10 为 Caps/Num/Scroll 锁定状态；`kModifierMask = 0x00FF`、`kLockStateMask = 0x0700`，`kInputStateMask = 0x07FF`，未知位由解析层直接拒绝。协议版本升到 3，ALPN 不变。
- 每个事件都携带发送时刻的完整输入状态，而不是只依赖 Down/Up 顺序。服务端注入器先对账再应用：位图有、held 没有的补按（按位顺序），held 有、位图没有的抬起（按按下反序）；修饰键自身的 Down/Up 也走同一套对账，重复或丢失的边沿不会造成二次按下或残留。
- 锁键是状态而不是按住的键：注入器在每次事件到来时把客户端的锁键位与本机状态（`GetAsyncKeyState`，注入线程无消息循环，`GetKeyState` 不可靠）比较，不一致就点按一次对应锁键（按下并立即抬起），并把这次切换记入 `lock_toggles`。注入器自己维护 `host_locks_`（首次使用时从系统读取），因此自己点按过的切换不会被下一事件重复点按；对端自己按下锁键时也会同步翻转该模型。dry-run 下模型从 0 开始，不读取也不改变真实桌面，回环计数可重复。
- AltGr：Windows 把它表现为合成的左 Ctrl + 右 Alt。注入器把同时出现的这两位折叠为右 Alt，并用 `altgr_synthetic_ctrl_` 记住；直到对端不再报告合成的 Ctrl 才解除。AltGr 松开时不会被位图重新按回一个多余的 Ctrl。
- 新增 `ControlType::InputRelease = 7`（无载荷）：客户端放弃控制、窗口失焦或退出时发送，服务端按已排队的顺序抬起注入器仍持有的键与鼠标键。它和 InputEvent 共用同一队列，因此不会与先到的释放事件乱序，也不会在断线前被丢掉。
- Flutter：`modifierBitsFor()` 从 `HardwareKeyboard.instance.physicalKeysPressed` 生成修饰键位，`lockBitsFor()` 从 `lockModesEnabled` 生成锁键位，鼠标与键盘事件都携带；`ViewerModifier` / `ViewerLock` 常量与协议位序一致。`_releaseRemoteInput()` 先按本地记录补发 Up，再发 `releaseInput` 兜底；页面注册了 `WidgetsBindingObserver`，窗口失去焦点（非 resumed）时同样执行该释放，控制开关保持开启，回到窗口后可继续操作。
- 关键坑：Flutter 的 `PhysicalKeyboardKey.usbHidUsage` 高 16 位是 usage page（修饰键实际为 0x000700E0），协议只接受低 16 位 usage。P2 的按键转发因此带着 0x0007 前缀，会被 bridge 的范围校验拒绝；现在按键采集与位图生成都先 `& 0xFFFF` 并确认 page 为 0x07。

### 测试与验证

- `protocol`：InputEvent 状态位往返（鼠标携带 Caps+Scroll，键盘携带 Right Alt）、未知位拒绝；注入器 dry-run 用例——完整 Shift+A（5 事件、5 次注入）、缺少 Shift Down（自动补按）、缺少 Shift Up（自动抬起）、AltGr 合成 Ctrl（4 事件、4 次注入）、锁键切换（2 次点按，连同 4 个按键事件共 8 次注入且无拒绝）、锁键已一致时不点按、`release_held()` 在不停止线程的情况下抬起 held 键、连续 10 次快速切换修饰键每次只注入一次；原有 held Ctrl 释放与 Pause 拒绝用例保留。
- `chast-quic verify`：脚本携带状态位图的 Shift+A，逐条比较状态位后全部 ok；随后发送 `InputRelease`，新增断言 `the viewer's release request reached the server`。
- Flutter `test/modifier_bits_test.dart`：八个修饰键 usage 的位映射、三种锁键状态位、非修饰键忽略、`tester.sendKeyDownEvent` 后 `physicalKeysPressed` 生成的位图。
- Release 构建同机演练（`serve --input-dry-run --dump-input`，脚本开启控制、移动、点击、关闭控制）：收到 4 个事件，dump 为 `state=0x000`，`input received=4 applied=4 rejected=0 failed=0 dropped=0 releases=1`。
- 本机自动化仍无法合成真实键盘事件（见 P2 记录），位图与锁键随真实按键的端到端回放需用物理键盘再确认一次。

### 已完成范围

- 修饰键位图与丢失 Down/Up 的自动修复（协议 v3）。
- Caps/Num/Scroll 状态同步（按需点按锁键）。
- 放弃控制、窗口失焦、退出/断线时的全量释放：客户端补发 Up + `InputRelease`，服务端注入器 `release_held()`，会话结束时 `stop()` 再释放一次。
- 快速切换与断线残留的注入器级测试；回环新增释放请求断言。

### 设计取舍

- 不做按键级“按下超时”：长时间按住（游戏、拖拽）是合法操作，按时间强行释放会打断它。真正卡住的情况是客户端消失或网络中断，这时 QUIC idle timeout（30 s）结束会话，`stop()` 的 held 释放兜底；短暂失焦由客户端主动释放。
- 锁键只在状态变化时点按，且注入器用 `host_locks_` 记录自己造成的切换，避免每个事件都重复点按。

