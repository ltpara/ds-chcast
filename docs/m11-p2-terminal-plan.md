# M11 P2：远程终端（chast 自有设计）

状态：协议 v27、传输、主机 ConPTY、查看器原生通道、CLI 验收与 Flutter
终端面板已完成（2026-10-27）；`ctest -C Release` 的 `quic_terminal` /
`quic_terminal_denied` 通过，`flutter analyze`、`flutter test`（303 项）
与 Release 构建通过。剩余：双机（含中继）验收与 GUI 点击复核。
目标：查看器在主机上开一个真实控制台（默认 `%COMSPEC%`，即 cmd.exe），
数据走现有 QUIC 会话，不新增监听端口。

## 1. 与 RustDesk 的差异与取舍

RustDesk 的终端（`src/server/terminal_service.rs`，约 2100 行）基于 portable-pty：
每个终端在服务端常驻、独立 helper 进程负责跨会话（服务模式）启动、带 1 MB 输出
缓冲与断线重连回放、SIGWINCH 两段式重绘、超过 512 B 压缩。它的定位是“持久终端
服务”，会话断开后进程继续活着。

chast 不照搬：

- 终端是**会话内的一次能力**，和画面、文件、端口转发同级：不常驻、不跨会话、
  不改认证。会话结束（或权限撤销）即关闭 ConPTY 与子进程。
- 数据面沿用端口转发的形状：**两条单向 QUIC 流** —— 查看器开上行（键盘输入），
  主机开下行（终端输出），头都是 16 字节 `CTTY`。QUIC 每条流自带流控，
  不压缩、不做应用层窗口。
- 不做断线重连回放：控制台输出不落盘、不重放。要保历史请用主机侧的重定向。
- 不做 helper 进程：主机程序在哪个会话，终端就开在哪个会话；服务模式（session 0）
  下终端同样在 session 0，不跳转到交互会话。这条写进限制。

## 2. 协议（v27）

版本号 26 -> 27。新增控制消息：

| 值 | 消息 | 方向 | 载荷 |
| --- | --- | --- | --- |
| 36 | TerminalRequest | viewer -> host | terminal_id u32, cols u16, rows u16 |
| 37 | TerminalResult | host -> viewer | terminal_id u32, code u8, reason_len u16, reason |
| 38 | TerminalStop | 双向 | terminal_id u32, code u8, reason_len u16, reason |
| 39 | TerminalResize | viewer -> host | terminal_id u32, cols u16, rows u16 |

`TerminalCode`：Ok=0、Refused=1（主机策略不允许）、Busy=2（终端数已满）、
Failed=3（ConPTY/子进程创建失败）、Cancelled=4（任一侧撤销）。

- 尺寸：cols 1..500、rows 1..200；越界拒绝而不是钳制。
- 权限位 `kPermissionTerminal = 0x0100`（0x0040 继续留空，见端口转发文档）；
  `kPermissionAll` 纳入。ServerInfo 增加 `kServerInfoTerminalEnabled = 0x0100`，
  `kServerInfoPermissionFlags` 同步。
- 每会话最多 2 个终端。

### CTTY 流头（16 字节）

```
magic "CTTY" (4) | version=1 (1) | reserved (3) | terminal_id u32 | reserved u32
```

- 查看器先发 TerminalRequest，收到 Ok 后开上行流并写头。
- 主机收到上行流时校验终端已登记、权限仍在；每个终端的下行流在**有输出时**才
  打开（惰性），带同样的头。方向本身区分上下行。
- 流 FIN = 该方向结束（主机侧等价于关闭 stdin/stdout）；abort = 终端失败。
- 接收队列高水位 256 KiB：到水位暂停该流的 QUIC 接收，排空到 128 KiB 恢复；
  绝不整包丢弃。

## 3. 主机侧

- `HostSettings.allow_terminal`，默认 **false**；设置键 `terminal`；
  同样并入 `permissions=` 掩码。
- `TerminalRelay`（`include/chast/terminal_relay.h`）：
  1. `CreatePseudoConsole`（Kernel32 动态加载，Win10 1809+），两个匿名管道；
  2. `CreateProcessW(%COMSPEC%)`，`PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`，
     `EXTENDED_STARTUPINFO_PRESENT`，`CREATE_UNICODE_ENVIRONMENT`，
     工作目录为主机默认；
  3. 读线程阻塞 `ReadFile` 输出管道 -> 有界输出队列（1 MiB），队列满就停读；
     写线程消费输入队列（256 KiB）阻塞 `WriteFile`；
  4. `resize(cols, rows)` -> `ResizePseudoConsole`；
  5. `close()`：先送一次 Ctrl+C 风格的 `\x03`？不做——直接
     `ClosePseudoConsole` 并 `TerminateProcess`，`WaitForSingleObject` 后关句柄。
- `HostServer` 会话循环：
  1. `take_terminal_request`：检查设置与权限，创建 relay，登记 id，回答
     TerminalResult（创建失败回答 Failed 并带 win32 错误码文本）；
  2. `pump()`：输出队列 -> `open_terminal_output`（首次）-> `send_terminal_data`，
     保持最多一个未发送块（64 KiB）；上行输入 -> relay 输入队列，队列满即暂停该流；
  3. 会话结束、权限撤销或 TerminalStop：关闭全部终端。
- 日志记录 `approved/spawned/closed` 与失败原因，不记录终端内容。
- 实测两个必须遵守的细节（2026-10-27）：
  1. 子进程的标准句柄必须显式置为无效（`STARTF_USESTDHANDLES` + 三个 NULL）。
     否则子进程沿用本进程的句柄：从 `chast-quic` 被重定向的 stdin 立刻读到
     EOF，cmd.exe 只把横幅打印到本进程的 stdout，约 0.2 s 后以 0 退出，
     ConPTY 上只剩初始化/退出转义序列。句柄无效时 cmd 自行打开本控制台的
     `CONIN$`/`CONOUT$`（即伪控制台），行为与“GUI 父进程”一致。
  2. TerminalStop 不得重置已经干净结束的方向。QUIC 不跨流排序，
     stop 可能早于下行数据到达；查看器在 `stopped` 后保留条目，直到
     `take_terminal_output()` 报 ended 且队列排空，主机侧则立即关控制台。

## 4. 查看器侧

- 原生 API：`request_terminal` / `take_terminal_result` / `open_terminal_input` /
  `send_terminal_input` / `finish_terminal_input` / `send_terminal_resize` /
  `take_terminal_output` / `take_terminal_input` / `send_terminal_stop` /
  `take_terminal_stop` / `close_terminal`。
- CLI：`chast-quic verify --terminal` 起内置主机（`--allow-terminal`），
  查看器请求 100x30 终端、写 `echo CHAST_TTY_OK` 与 `exit` 两行并读回输出，
  断言输出含标记（回显 + 命令结果各一次）且以 Ok 收尾；`--terminal-denied`
  验证闸门关闭时被拒。
- GUI：会话工具栏“终端”按钮（仅当 HelloAck 宣布 `terminal_enabled` 时可点）
  打开模型态面板。面板自己持有屏幕模型与输出轮询，页面只决定是否显示：
  - 等宽渲染（Consolas / Cascadia Mono / Courier New 回退），按面板实际尺寸
    申请并防抖重设 cols×rows（120 ms，1..500 × 1..200），30 ms 轮询
    `take_terminal_output`；
  - 键盘走 `terminalKeySequence`（方向键/Home/End/F1-F12/Ctrl 字母/Alt 前缀，
    `Ctrl+Shift+C/V` 留给本地），Ctrl+V / Shift+Insert 粘贴并把换行规范为
    `\r`，主机开启 bracketed paste 时用 `\x1b[200~…\x1b[201~` 包裹；
  - 与聊天面板互斥；面板获得焦点时页面暂停远程键鼠转发
    （`setKeyCapture(false)`），离开后恢复；Escape 在面板内作为 `\x1b`
    发给主机，页面持有焦点时才关闭面板；
  - 关闭按钮发送 TerminalStop 并结束主机控制台；主机结束、权限撤销或申请
    失败时保留最后屏幕并显示原因，可“重新申请”。
  - 代码：`app/lib/terminal/terminal_model.dart`（VT 解析、屏幕、回滚、
    备用屏、宽字符）、`app/lib/terminal/terminal_keys.dart`（键序列）、
    `app/lib/widgets/terminal_panel.dart`（面板）、`app/lib/pages/session_page.dart`
    （显示与焦点仲裁）。

## 5. 测试与验收

- 协议单测：四个消息的编解码、尺寸越界、非法 UTF-8、CTTY 头。
- `verify --terminal`：真实 ConPTY + cmd.exe（或 `%COMSPEC%`），断言标记字节、
  结束码与清理；`--terminal-denied` 负向。两者已注册为 ctest
  `quic_terminal` / `quic_terminal_denied`（单机实测各约 31 s）。
- Flutter（2026-10-27）：`flutter test` 新增 24 项 VT 解析/键序列单测
  （光标、擦除、SGR 与 256/真彩、宽字符覆盖、滚动区、备用屏、DSR 回复、
  resize 裁剪/补齐、UTF-8 分片与坏字节、方向键/F 键/组合键）与 3 项面板
  widget 测试（按布局尺寸申请、真实输出上屏、焦点内按键发送、结束后
  停止输入并提示）；全量 **303 项通过**，`flutter analyze lib test`
  无问题，`flutter build windows --release` 成功。
- 复跑（2026-10-27，面板合入后）：`ctest -C Release -R quic_terminal`
  **2/2 通过**（`quic_terminal` 32.43 s、`quic_terminal_denied` 30.83 s），
  真实 ConPTY + cmd.exe 路径未受查看器改动影响。
- 未自动化的验收：真实会话里的面板点击/键盘输入需要交互式桌面，本轮以
  widget 测试 + 既有 CLI 真链路验收覆盖；GUI 点击复核与双机/中继验收
  仍待做。
- 边界：终端数上限、重复 id、主机创建失败、会话中途撤销权限、超长输入。
- 限制：不做回放（主机结束或权限撤销后保留最后屏幕）；无 IME 组合输入，
  组合中的候选窗不上屏，但已提交的 Unicode 字符可输入；宽字符按 2 列近似，
  组合 emoji 对齐可能略偏；终端数与尺寸沿用协议上限。
- 不做：UDP/串口、端口转发式的任意命令指定（终端本身就是任意命令）、
  断线重连回放、跨会话 helper、压缩。
