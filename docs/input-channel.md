# 输入通道开发记录

本文记录 chast 远程控制输入通道的设计与实现进度，按阶段（P1/P2/P3）更新。

## 目标与分工

- 协议层与平台无关：坐标用**远端虚拟桌面像素**，按键用 **USB HID usage**。
- 被控端（服务端）负责把事件真正注入本机；客户端只负责采集、映射、发送。
- 输入默认关闭，必须由服务端显式开启，并在 HelloAck 中告知客户端。

## P1 — 协议 + 服务端注入（已完成）

### 协议（`chast/protocol.h`，版本 2）

控制流新增 `ControlType::InputEvent = 6`，载荷固定 16 字节：

| 字段 | 类型 | 说明 |
|---|---|---|
| kind | u8 | 1 = 鼠标，2 = 键盘 |
| action | u8 | 鼠标：Move/MoveRelative/ButtonDown/ButtonUp/Wheel/WheelHorizontal；键盘：Down/Up |
| buttons | u16 | 鼠标按键位（左 1、右 2、中 4、后 8、前 16）；键盘为 0 |
| x | i32 | 绝对事件为虚拟桌面像素；相对移动为增量；横向滚轮为 WHEEL_DELTA 单位 |
| y | i32 | 同上；纵向滚轮用 y |
| hid_usage | u16 | 键盘专用，usage page 0x07 |
| reserved | u16 | 0 |

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

## P2 — Flutter 端采集与坐标映射（进行中）

计划：

- 会话页把指针/键盘事件换算成"帧像素 + 虚拟桌面原点"，通过 method channel 交给 `ViewerEngine`，由其调用 `QuicSession::send_input()`。
- 键位取 `physicalKey.usbHidUsage`，不经过字符映射。
- 焦点/鼠标捕获、远程画面接管本地指针显示。

## P3 — 修饰键、锁定键与卡键保护

计划：

- 每个按键事件携带修饰键状态，服务端补齐/抬起差异，AltGr 走 Ctrl+Alt 特例。
- Caps/Num/Scroll 状态同步（必要时临时切换锁键）。
- 超时清理与退出时全量释放（P1 已在注入器层做了释放与 held 记录，P3 补策略）。
