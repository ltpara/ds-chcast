# M8 — 发射端加固计划

更新时间：2026-10-24
分支：`feat/m7-security`（M7 未提交的收尾与 M8 工作在同一分支继续）
基线：M7 完成；M8.0（协议 v14）、M8.1（协议 v15）、M8.4（协议 v17）、
M8.5（协议 v18，Opus + 麦克风上行）、M8.6（协议 v19，锁屏/UAC 与隐私模式）
已完成
参考：`D:\rustdesk` @ `c7f212d`（只做能力对照，不复制实现）

## 状态标记约定

| 标记 | 含义 |
| --- | --- |
| `[x]` | 已完成，有构建/测试/实机数据 |
| `[~]` | 进行中，代码已开始改 |
| `[ ]` | 未开始 |
| `[!]` | 阻塞或待决策 |

每个里程碑在「开发记录」里按日期追加一行，写清改了什么、验证了什么、还剩什么。
未验证的结论一律标「未验证」，不用「应该可以」。

## 1. 背景与目标

2026-10-23 对照 RustDesk 发射端（被控端）代码后，chast 的主链路不落后：
零拷贝、QUIC/TLS、三杠杆 ABR、分辨率自适应、延迟探针都已具备。差距集中在
被控端的产品面与异常场景。M8 只做发射端，目标是让现有能力在真实环境里
「可用、耐用」，再补新服务。

设计约束不变：

- 采集、转换、编码必须是同一张 GPU，全程零拷贝；不引入 CPU RGBA/NV12 回读。
- 传输是 QUIC，不引入 KCP/TCP 作为主路径。
- 参考 RustDesk 的行为，但协议、驱动和实现都自己设计。
- 协议字段只增不改，版本号随语义提升，ALPN 保持 `chast/1`。

明确不做（本阶段）：软件编码、GDI 回退、跨平台、KCP、RustDesk 代码搬运。

## 2. 发射端差距清单

| 能力 | chast 现状 | RustDesk | 优先级 |
| --- | --- | --- | --- |
| 会话内切换显示器 | 需断开重连（`host_server.cc` Hello 时固定输出） | `SwitchDisplay` 会话内切换 | P0（M8.0） |
| 远程修改分辨率 | 无 | `change_resolution` + 虚拟屏 | P0（M8.1） |
| 无头/虚拟显示器 | DXGI 无输出即建不了采集器 | IDD / usbmmidd 虚拟屏 | P0（M8.2） |
| 编码后端覆盖 | oneVPL + FFmpeg 硬件兜底（NVENC/AMF/QSV） | NVENC/QSV/AMF/VAAPI | P1（M8.3） |
| 富剪贴板/文件传输 | 纯文本 256 KiB | 文本/HTML/RTF/图片/SVG/文件 | P1（M8.4） |
| 麦克风/双向音频 | 仅系统声音 AAC-LC | loopback + 麦克风 + Opus | P2（M8.5） |
| 锁屏/UAC | 注入被系统丢弃 | 检测 prelogin、切换输入桌面 | P2（M8.6） |
| 隐私模式 | 无 | 四种实现 | P2（M8.6） |
| 安装/更新/i18n | 无 | MSI、updater、多语言 | P3（M8.7） |

## 3. 里程碑

- `[~]` **M8.0 会话内显示器切换**：协议 v14 + 主机重建 + 查看器菜单，
  代码与本机可覆盖的验收已完成；双屏人工切换仍待实机（见 4.4）。
- `[x]` **M8.1 远程分辨率调整**：查看器选择主机输出支持的显示模式；
  主机侧保存原模式并在会话结束时恢复。需要真实的模式枚举/回滚和权限约束。
  协议 v15，方案见第 5 节。真实路径回环已通过（第 5.5、6 节）；查看器菜单
  的人工点选与指针视觉复核待实机。
- `[!]` **M8.2 虚拟显示器与无头主机（暂缓）**：自研 IDD 驱动需要 WDK、
  测试签名（本机要 `bcdedit` + 重启）和发布签名（EV/attestation）；开源
  IddCx 示例同样绕不开签名，未签名驱动在强制驱动签名的系统上无法加载。
  2026-10-23 决定暂缓，等 M8.7 安装器/签名链路具备后再评估；本轮不引入
  驱动依赖，也不复制 RustDesk 驱动。
- `[~]` **M8.3 编码后端 NVENC/AMF**：新增 vcpkg FFmpeg 硬件后端
  （NVENC/AMF/QSV），Auto 先试 oneVPL、失败再试 FFmpeg；D3D11VA 帧池
  与同卡 GPU 拷贝保持零拷贝，无软件编码。QSV 已在本机完成编码与回环
  验证；NVENC/AMF 缺实机，仅编译与可用性检查通过（见第 9 节）。
- `[~]` **M8.4 富剪贴板与文件传输**：协议 v17；HTML/RTF/DIB、查看器
  上传、续传/摘要、GUI 1 GiB 验收已完成（见 6.6、9）。文件传输现为
  双向：主机可经托盘把文件发到查看器，查看器支持工具栏选择与资源
  管理器拖放上传，回环探针覆盖两个方向。双机浏览器富剪贴板人工核对
  与双机 GUI 文件往返仍待实机。
- `[x]` **M8.5 麦克风与双向音频**：协议 v18；系统声音与麦克风统一走
  Opus；查看器麦克风上行到主机扬声器，与主机系统声音下行构成双工。
  协议、Opus 编解码、传输层 `CMIC`、主机侧接入、查看器采集/桥接/
  Flutter 工具栏与 `verify --microphone` 探针均已完成（见 7.7、9）；
  真实麦克风与双机回声验收待实机。AEC/噪声抑制单独评估。
- `[x]` **M8.6 锁屏/UAC 与隐私模式**：协议 v19；主机每秒跟随输入桌面，
  能打开安全桌面的服务态继续采集/注入，托盘态打不开时暂停而不是断开会话；
  隐私模式用 `WDA_EXCLUDEFROMCAPTURE` 的置顶穿透遮罩遮挡本机屏幕，采集
  不受影响。虚拟显示器按 M8.2 结论继续暂缓。回环 `verify --privacy` 与
  关闭闸门探针已通过（第 8 节）；真实锁屏/UAC、服务态安全桌面与遮罩
  视觉仍需实机。
- `[ ]` **M8.7 安装包、自动更新与 i18n**：MSI/签名、更新通道、界面多语言。

## 4. M8.0 会话内显示器切换

### 4.1 协议 v14

版本从 13 升到 14；ALPN 不变。新增两条控制消息：

| 类型 | 方向 | 载荷 |
| --- | --- | --- |
| `SwitchDisplay = 15` | 查看器 → 主机 | `u16 display_index`，`kDisplayAuto` 表示回到主机配置的默认输出 |
| `DisplaySwitchResult = 16` | 主机 → 查看器 | `u16 requested_index`、`u8 code`、`u16 reason_len`、UTF-8 reason（≤256 B） |

结果码：

| code | 名称 | 含义 |
| --- | --- | --- |
| 0 | Ok | 请求已被接受；已经在推该输出，或切换完成并随后发送 `StreamInfo` |
| 1 | NotFound | 索引越界或输出已不在 |
| 2 | Failed | 新采集器/编码器创建失败，旧流保持 |
| 3 | Busy | 距上次切换不足 1 s，或正在切换 |

成功后的实际参数（origin、尺寸、显示器列表、编码器、码率）仍以
`StreamInfo` 为准；`DisplaySwitchResult(0)` 只表示请求被接受。失败不会
发送 `StreamInfo`，流保持原样。

### 4.2 主机流程

1. 控制流解析请求，存入待处理槽；主机线程在帧循环开头取走。
2. 重新枚举显示器，按当前列表解析索引（热插拔后索引可能变化）。
3. `kDisplayAuto` 解析为设置里的 `display`，否则主屏。
4. 无效索引 → `NotFound`，继续推原流。
5. 与当前输出相同 → `Ok`，不重建。
6. 不同 → **先建新资源、成功后再换**：
   - 用同一 `CaptureConfig` 创建新采集器，等待可用帧（切屏超时 3 s，
     不是启动时的 30 s）；
   - 沿用当前分辨率档位调用 `set_nv12_scale`；
   - 用新采集器的适配器与尺寸创建编码器，按配置码率启动，再用 ABR
     预算裁剪；
   - 全部成功后交换 `capturer`/`encoder`，刷新 `stream_info`、发送
     `StreamInfo`、强制两个 IDR、重置光标状态与输入重复帧标记；
   - 任一环节失败：释放新资源，保留旧流，回 `Failed`。
7. 更新 `HostStatus` 的 `display_index` / `display_name` / `stream`，写日志。

限制：1 s 内重复请求回 `Busy`；切换只允许在 `kPermissionInput` 有效的
会话里进行（只看模式不能改变主机画面）。

### 4.3 查看器

- `ViewerEngine::SwitchDisplay(index)` 把请求排到会话线程，由会话线程
  调用 `QuicSession::request_display()`，避免跨线程持有 QUIC 句柄。
- `ViewerStatus` 增加 `display_switches`（成功计数）、`display_switch_failures`
  （拒绝计数）与 `display_switch_error`（最近一次失败原因，成功后清空）。
  两个计数器都要有：只比错误文本无法区分两次相同的拒绝原因。
- Flutter 菜单从「带 display 参数重连」改为「会话内切换」：
  - 串流中且主机授予输入权限时走 `ViewerBridge.switchDisplay`；
    未串流或原生侧拒绝排队时仍回退到重连路径；
  - 等待期间工具栏标记目标输出（`_pendingDisplay`）；成功计数增加后
    把它提交为新的 `_display`，失败计数增加后回退并在 SnackBar 显示原因；
  - 成功时依赖 `StreamInfo` 更新 origin/尺寸/显示器列表，指针映射不变。

### 4.4 验收标准

- `[x]` `chast-protocol-test`：两条消息往返；截断/超长/未知结果码被拒绝。
- `[x]` `chast-quic verify`：串流中请求当前输出得到 `Ok` 且流继续；越界索引
  得到 `NotFound` 且视频不断；断言失败计数与流更新计数。回环请求安排在
  1.2 s / 2.4 s，避开主机 1 s 的切换限流。
- `[x]` `ctest -C Release` 10/10 全过；Flutter `analyze` 无问题、
  `test` 63 项全过（含新增的切屏计数器解析用例）、Windows Release 构建通过。
- `[~]` 真实双屏（人工）：切换后画面、origin、尺寸、指针位置同时变化，无重连；
  切换期间最多丢 1–2 帧；切回原屏正常。
- 本机只接了 1 个输出（1920×1080），因此跨输出重建采集器/编码器的路径
  **未验证**；同输出 no-op 与越界拒绝已由回环覆盖。

### 4.5 风险

- 新旧采集器短暂共存，显存峰值上升；切换完成后立即释放旧资源。
- 新输出首帧可能迟迟不来；3 s 超时后回滚，不影响旧流。
- 热插拔会让索引指向别的输出；每次切换重新枚举，不缓存旧列表。
- 分辨率档位跨屏保留；若新屏不支持缩放，`set_nv12_scale` 失败则按
  100% 重建并记录日志。

## 5. M8.1 远程分辨率调整

### 5.1 协议 v15

版本从 14 升到 15；ALPN 不变。新增两条请求/响应对：

| 类型 | 方向 | 载荷 |
| --- | --- | --- |
| `ListDisplayModes = 17` | 查看器 → 主机 | `u16 display_index` |
| `DisplayModes = 18` | 主机 → 查看器 | `u16 display_index`、`u8 code`、`u16 reason_len`、UTF-8 reason（≤256 B）、`u16 count`、count × (`u16 width`、`u16 height`、`u16 refresh_hz`、`u16 flags`) |
| `SetDisplayMode = 19` | 查看器 → 主机 | `u16 display_index`、`u16 width`、`u16 height`、`u16 refresh_hz` |
| `DisplayModeResult = 20` | 主机 → 查看器 | `u16 display_index`、`u16 width`、`u16 height`、`u16 refresh_hz`、`u8 code`、`u16 reason_len`、UTF-8 reason（≤256 B） |

结果码与 `DisplaySwitchCode` 同构：

| code | 名称 | 含义 |
| --- | --- | --- |
| 0 | Ok | 列表有效；或设置完成，随后发送 `StreamInfo` |
| 1 | NotFound | 输出不存在，或该输出不支持请求的模式 |
| 2 | Failed | 驱动拒绝模式、采集/编码器重建失败；原模式已回滚 |
| 3 | Busy | 距上次成功切换不足 1 s，或正在处理 |

约束：

- `DisplayModes` 的 width/height 是**桌面空间**：90/270 旋转面板会交换宽高，
  与查看器看到的画面一致；主机在应用前再换回面板原生方向。
- 列表只含 32 bpp、非隔行、refresh 1..1000 的模式，按 (width, height,
  refresh) 去重；排序为大画面优先、同尺寸高刷新优先。`flags` bit0 表示当前
  模式，其余位必须为 0。
- `count` 上限 `kMaxDisplayModes = 256`；reason 上限 256 B，在 UTF-8 字符
  边界截断。
- 只允许修改**正在串流的输出**；索引在每次请求时按最新枚举解析。
- 查询列表不要求输入权限；修改模式要求 `kPermissionInput`，与切屏一致。
- 成功后的实际 origin/尺寸仍以 `StreamInfo` 为准；`DisplayModeResult(0)`
  只表示请求被接受。

### 5.2 采集层

新增 `include/chast/display_mode.h` + `src/capture/display_mode.cc`：

- `list_display_modes(display)`：`EnumDisplaySettingsExW`，过滤/去重/排序，
  `ENUM_CURRENT_SETTINGS` 标记当前项；旋转 90/270 时输出交换后的宽高。
- `DisplayModeChange`：
  - `save(display)`：保存当前 `DEVMODEW` 的原生宽高与刷新率；
  - `apply(mode)`：以当前 DEVMODE 为底，只覆盖宽高/刷新率，调用
    `ChangeDisplaySettingsExW(device_name, &dm, nullptr, 0, nullptr)`——
    临时模式，不写注册表；失败返回 `DISP_CHANGE_*` 名称；
  - `restore()`：重新查询当前模式，只有仍等于我们应用的模式时才恢复，
    操作员在会话中手动改过就放弃恢复；
  - 应用后记录驱动实际生效的模式用于比较。

不改变位置、旋转、DPI、位深与 HDR；不做注册表持久化。

### 5.3 主机事务

1. 控制流存单槽请求；帧循环开头取走。
2. 校验输入权限、1 s 限流、索引必须是当前串流输出、模式必须在最新列表。
3. 目标等于当前模式 → `Ok`，不改动、不发 `StreamInfo`。
4. 第一次成功修改前保存原模式；`ChangeDisplaySettingsExW` 失败 → `Failed`。
5. 模式变化会让 DXGI duplication 失效：同步 `capturer->reinitialize()`，
   等待可用帧（3 s，复用切屏超时），用新帧尺寸创建编码器并恢复当前
   ABR 码率/帧率。模式切换后驱动短时间内会让 `DuplicateOutput` 返回
   `E_ACCESSDENIED`，重建在 3 s 窗口内按 100 ms 间隔重试，而不是首次失败
   就回滚。
6. 成功：交换编码器、重新枚举 `attached`、刷新 `stream_info`、发送
   `StreamInfo`、强制两个 IDR、重置光标与重复帧状态，回 `Ok`。这里的
   `attached` 必须是**应用新模式之后**的枚举结果，否则公告里的显示器 rect
   仍是旧尺寸。
7. 任一步失败：恢复原模式，再 `reinitialize()`，丢弃编码器让帧循环按原
   尺寸重建并重公告，回 `Failed`；恢复也失败则结束会话并带错误。
8. 切到别的输出前，先恢复当前输出的原模式（旧采集器即将弃用，它进入
   access-lost 由帧循环重建）。
9. `Run` 内用 RAII 守卫在所有退出路径恢复；恢复失败只写日志。

顺手修正：`!encoder` 重建分支重新枚举显示器后没有把结果赋给 `attached`，
后续 `StreamInfo` 可能带旧 rect；M8.1 一并改为 `attached = refreshed`。

### 5.4 查看器

- `ViewerEngine::RequestDisplayModes(display)` 与
  `SetDisplayMode(display, width, height, refresh)` 单槽排队，由会话线程
  调用 `QuicSession`。
- `ViewerStatus` 增加：`display_modes`（含 width/height/refresh/current）、
  `display_modes_display`（列表所属输出）、`display_mode_changes`、
  `display_mode_failures`、`display_mode_error`。UI 用两个计数器区分两次
  相同原因的失败。
- Flutter 工具栏新增分辨率菜单：只对当前串流输出展示；打开前/进入会话后
  请求列表，当前模式打勾；选择后标记 pending，成功清空，失败 SnackBar
  显示原因。分辨率变化由 `StreamInfo` 驱动，指针映射继续使用 source 空间。

### 5.5 验收标准

- `[x]` `chast-protocol-test`：四条消息往返；截断、超长 reason、超模式数、
  未知 code、flags 保留位被拒绝。
- `[x]` `chast-quic verify --display-mode`：真实路径查询模式 → 切到另一
  模式 → 断言 `StreamInfo` 源尺寸变化且视频继续 → 切回原模式 → 断言恢复
  → 再切到新模式后直接结束会话，断言主机退出守卫把桌面恢复原模式。
  探针默认关闭，避免普通回环改动操作员屏幕。
- `[x]` `ctest -C Release` 10/10；Flutter `analyze` 无问题、`test` 65 项
  通过、Release 构建通过。
- `[~]` 实机：回环已证明切换期间视频流无序号缺口、退出后桌面恢复原模式；
  查看器菜单的人工点选和指针随 source 尺寸的视觉确认仍未验证（本机为
  单输出 1920×1080，浮层交互未做人工复核）。
- 失败/越界/无权限路径用回环覆盖；无第二输出或无多模式的环境只能覆盖
  查询与拒绝路径。

### 5.6 风险

- 临时模式不写注册表，但进程被强杀时不会执行恢复，桌面会停在新模式直到
  重新登录或手动改回；正常退出、错误退出和切屏路径都有恢复。
- `ChangeDisplaySettingsExW` 可能阻塞数秒；主机线程在此期间不发帧，控制
  消息排队，超时后回滚。
- 修改一个输出的模式可能让**其它**输出的 duplication 也失效；帧循环的
  ReinitNeeded 路径已覆盖，重建后自动重公告。
- 部分驱动枚举出的模式实际不可设；应用失败按 `DISP_CHANGE_*` 名称回报，
  不进入重建。
- 同分辨率换刷新率也触发 access lost，但 source 尺寸不变，查看器不需要
  重建解码器，只更新当前模式标记。
- 重建采集与编码器期间主机帧循环停顿约 1.5 s，64 ms 一帧的音频队列会丢
  几包（本机实测 4 包）；线上无丢包/乱序，音频错误计数为 0。若要把这段
  停顿也盖住，需要加大队列或把音频发送移出帧循环，代价是网络抖动时的
  音频延迟，留待 M8.5 一并评估。

## 6. M8.4 富剪贴板与文件传输

目标：剪贴板在文本之外支持 HTML、RTF 与 DIB 位图；查看器可以向主机
上传文件，主机也可以把文件发给查看器，两个方向都支持断点续传和
SHA-256 校验。查看器接受工具栏选择与资源管理器拖放，主机经托盘的
“发送文件到查看器…”选择文件。

### 6.1 协议 v16 → v17

版本从 15 升到 16，文件传输双向化时升到 17；ALPN 不变。控制消息继续
只承载协商与元数据，剪贴板内容和文件字节走各自独立的单向流，避免
1 MiB 控制载荷上限，也避免大块数据阻塞输入/切屏消息。v17 没有新增
消息：同四条文件消息把角色对调即可，Offer 由发送方发出，Answer/Result
由接收方回答，Cancel 任一方都可发出；`CFIL` 由发送方打开。

新增权限位与能力标记：

| 名称 | 值 | 含义 |
| --- | --- | --- |
| `kPermissionFileTransfer` | `0x0008` | 会话内允许文件传输（两个方向） |
| `kPermissionAll` | `0x000F` | 现有权限位并集 |
| `kServerInfoFileTransferEnabled` | `0x0008` | HelloAck/StreamInfo 声明可用 |
| `kServerInfoKnownFlags` | `0x000F` | 未知位仍被拒绝 |

新增控制消息：

| 类型 | 方向 | 载荷 |
| --- | --- | --- |
| `FileOffer = 21` | 发送方 → 接收方 | `u32 transfer_id`、`u64 size`、`u8 flags`、32 B SHA-256、`u16 name_len`、UTF-8 name（≤512 B） |
| `FileAnswer = 22` | 接收方 → 发送方 | `u32 transfer_id`、`u8 code`、`u64 resume_offset`、`u16 reason_len`、reason（≤256 B） |
| `FileResult = 23` | 接收方 → 发送方 | `u32 transfer_id`、`u8 code`、`u64 received`、`u16 reason_len`、reason（≤256 B） |
| `FileCancel = 24` | 任一方 → 对方 | `u32 transfer_id`、`u8 code`、`u16 reason_len`、reason（≤256 B） |

`FileOffer.flags` bit0 表示查看器可按 `resume_offset` 续传，其余位必须为 0。
结果码统一为：0 Ok、1 Refused、2 Busy、3 NotFound、4 DigestMismatch、
5 IoError、6 Cancelled。`FileAnswer` 只使用 0/1/2；`FileResult` 使用
0/4/5/6；`FileCancel` 使用 6。

剪贴板流 `CCLB`（双向单向流，一条流承载一次完整内容后 FIN）：

```
"CCLB" | u8 version=1 | u8 format_count | u16 reserved=0
       | u64 hash | u64 total_len
format_count × (u8 format | u8 reserved | u16 reserved | u32 length | bytes)
```

`format`：0 Text（UTF-8）、1 Html（CF_HTML，UTF-8）、2 Rtf（ASCII/UTF-8）、
3 Dib（CF_DIB 原始字节，含 BITMAPINFOHEADER）。`hash` 是 SHA-256 的前
8 字节，输入为按 format 升序的 `(u8 format, u32 length, bytes)`；用于
回环抑制，不用于安全校验。`total_len` 是全部 format 载荷之和，接收端
先校验再分配内存。

上限：Text 256 KiB、Html/Rtf 各 4 MiB、Dib 32 MiB、单条流总量 40 MiB、
format 数 4。超限时接收端中止该流并保留上一条剪贴板内容，不断开会话。

文件数据流 `CFIL`（发送方 → 接收方的单向流，每个传输一条；上传时由
查看器打开，下载时由主机打开）：

```
"CFIL" | u8 version=1 | u8 reserved[3] | u32 transfer_id
       | u64 offset | raw bytes ... (以 FIN 结束)
```

`offset` 必须等于 `FileAnswer.resume_offset`，字节数必须等于
`size - offset`；接收方边收边写，流结束（FIN）后校验长度和 SHA-256，
再原子改名。协议单文件上限 16 GiB，主机接收默认再限 4 GiB。

### 6.2 QuicSession

- 服务端不再只接受第一条客户端双向流：继续把第一条双向流当控制流，
  其余客户端单向流按前 4 字节分类，`CCLB`/`CFIL` 接受，未知魔数中止。
- 客户端接受多条服务端单向流，`CFRM`/`CAUD` 之外的 `CCLB` 进入剪贴板
  队列；每条流只允许一个魔数，重复或超限即中止。
- 新增 `send_clipboard_blob()`/`take_clipboard_blob()`：单向流写
  `CCLB` 头 + 载荷 + FIN；发送队列按“最新一条覆盖未发送的旧内容”，
  已发送的流不回收。
- 新增文件 API：`offer_file()`、`take_file_answer()`、`take_file_result()`、
  `cancel_file()`、`send_file_data()`、`take_file_chunk()`、`file_progress()`。
  文件数据按 64 KiB 分块；查看器用 `try_send_file_data()` 非阻塞写，
  发送队列满时返回 `WaitTimeout`，会话循环下一轮重读同一块；主机在
  帧循环里非阻塞取块，应用缓冲采用 32 MiB 高水位/8 MiB 低水位，
  超水位时消费 0 字节并暂停该流接收，靠 QUIC 流控把背压传回查看器；
  64 MiB 硬上限只作最后防线。文件流与视频/输入流互不阻塞。
- 剪贴板 blob 队列上限 8 条/40 MiB，文件应用缓冲 64 MiB；所有接收均
  先校验头再读字节，避免恶意长度导致大块分配。畸形或超限的 `CCLB`
  只中止该流并保留上一条内容，不断开会话。

### 6.3 共享库与平台层

新增两个静态库，主机与查看器都链接：

- `chast.clipboard`：纯内容模型与容器编解码
  （`ClipboardContent`、`encode_clipboard_stream`、`decode_clipboard_stream`、
  `content_hash`），加 `clipboard_win.h/.cc` 的 Win32 读写。
- `chast.transfer`：文件名校验、唯一命名、`.part` 接收器、增量 SHA-256、
  磁盘空间检查。依赖 `chast.security` 的增量哈希。

把 `src/host/clipboard.cc` 的文本实现迁移到 `chast.clipboard`，旧
`read_clipboard_text()`/`write_clipboard_text()` 保留为薄封装，供测试与
CLI 使用。`security.h` 增加 `Sha256` 增量类，单次 `sha256()` 改为其封装。

Win32 剪贴板：

- 读：`CF_UNICODETEXT`、注册格式 `HTML Format`、`Rich Text Format`、
  `CF_DIB`，逐个复制到内存；`OpenClipboard` 失败按 20 ms 重试 10 次。
- 写：`EmptyClipboard` 后一次写入所有可用格式，最后补 Text；
  `GlobalAlloc(GMEM_MOVEABLE)` + `SetClipboardData`，失败回滚并保留原
  UUID 引用。写入自己刚收到的内容后记录 `hash` 一次，下一次轮询跳过，
  防止两端乒乓。
- HTML 原样保存 CF_HTML 头（`Version`/`StartHTML` 等偏移），不做重写。

文件接收：

- 文件名清洗：去掉路径分隔符、`..`、控制字符、结尾点和空格，拒绝
  `CON/PRN/AUX/NUL/COM1-9/LPT1-9`（含带扩展名形式），非法字符替换为
  `_`，空名回退 `file`；长度按 UTF-16 ≤ 200 字符。
- 唯一命名：目标已存在时追加 ` (1)`、` (2)`；最终名在 Offer 时确定，
  数据写入 `<final>.part`。
- 续传：`.part.chast` 边车记录 `size + SHA-256`，与 Offer 一致时从现有
  长度续传并把已有字节喂给哈希；否则删除重来。摘要不符删除 `.part`，
  回 `DigestMismatch`。
- 写完后 `FlushFileBuffers`，再用 `MoveFileExW` 改名；路径统一加
  `\\?\` 前缀，拒绝 reparse point，接收目录不存在时创建。
- 接收目录：默认 `%USERPROFILE%\Downloads\chast-incoming`（服务态为
  `%ProgramData%\chast\incoming`），设置里可覆盖。磁盘剩余空间不足
  `size + 64 MiB` 时拒绝。

### 6.4 主机

- 设置新增 `allow_file_transfer`（默认开）与 `file_receive_dir`（默认空，
  按上节解析）、`file_transfer_max_mb`（默认 4096）。HelloAck 的
  `kServerInfoFileTransferEnabled` 与权限位随之联动。
- 剪贴板：500 ms 轮询改为读取完整 `ClipboardContent`，按 `hash` 去重；
  有变化时 `send_clipboard_blob`。收到 blob 后解码、校验上限、写入本机
  剪贴板，并把该 hash 记为“自己写入”，避免回传。
- 文件：Offer 校验权限、大小、剩余空间、文件名；选唯一名，检查/建立
  `.part`，回 `FileAnswer`。帧循环驱动 `take_file_chunk()` 写入接收器，
  每 64 KiB 更新进度；FIN 后校验并回 `FileResult`。中途会话结束或
  `FileCancel` 时关闭 `.part`，保留边车以便下次续传。
- 统计：新增 `files_received`、`file_bytes`、`file_failures`、
  `file_resume_bytes`、`clipboard_rich_sent/received`；`HostStatus` 暴露
  当前传输 `transfer_id/name/size/received`，供托盘显示进度。
- 主机→查看器下载：`HostServer::start_file_download(path)` 由托盘
  “发送文件到查看器…”（`GetOpenFileNameW`）排队；哈希在独立线程完成，
  帧循环按 8 ms/轮、32 MiB/s 令牌桶发送，队满时从 `sent` 偏移重读。
  `HostStatus` 增加 `download_active/name/size/sent/error`。
- 审批窗口与设置页加入“文件传输”开关；`set_permissions()` 降低权限时
  立即取消进行中的传输并回 `Cancelled`。

### 6.5 查看器

- 原生侧用共享 `chast.clipboard` 读写富剪贴板，会话线程每 500 ms 轮询；
  文本轮询与 Flutter 文本桥退役，改由原生队列统一处理。
- 文件选择用 `GetOpenFileNameW`（comdlg32，系统 DLL，不引新插件），
  一次一个文件；`ViewerEngine::SendFile(path)` 计算 SHA-256、发 Offer，
  收到 `FileAnswer` 后从 `resume_offset` 打开文件流式发送。
- `ViewerStatus` 增加 `file_state`（idle/offered/sending/done/failed）、
  `file_name`、`file_size`、`file_sent`、`file_error`，以及剪贴板富格式
  计数。Flutter 工具栏加“发送文件”，底部显示进度条与失败 SnackBar。
- 收到主机不支持/拒绝/摘要错误时保留本地文件，允许重试。
- 资源管理器拖放：`DragAcceptFiles` 挂在 `FLUTTERVIEW` 子类窗口，
  `WM_DROPFILES` 取第一条路径经 `droppedFile` 交给 Dart，与工具栏
  共用同一上传流程。
- 下载接收：`ViewerEngine` 用共享 `FileReceiver` 把主机文件写到
  `Downloads\chast`，边收边更新 `ViewerStatus` 的
  `download_phase/name/size/received/error` 与 `files_received/
  file_receive_failures`；Flutter 显示接收进度条，完成/失败弹提示。

### 6.6 验收标准

- `[x]` `chast-protocol-test`：四条文件消息与 `CCLB` 容器往返；截断、
  超长、未知 code/flags、哈希不符、超上限、format 重复/乱序被拒绝。
- `[x]` `chast-quic verify`：`verify` 始终覆盖富剪贴板（不需要单独的
  `--rich-clipboard` 开关）。四格式经 `CCLB` 往返，断言哈希、内容与
  计数；再注入声明总量超限的 blob，断言会话不断、旧内容保留，且后续
  有效剪贴板仍能送达。
- `[x]` `chast-quic verify --file-upload`：上传 300 KiB 探针文件，断言
  Offer/Answer/Result、落盘摘要一致、字节数一致；截断后续传
  （`resume_offset = 102400`）；篡改摘要断言 `DigestMismatch` 且
  `.part` 被删除。更大数据量的实测见下方的 GUI 8 MiB 与 1 GiB。
- `[x]` `chast-quic verify --file-download`：同一探针反向运行。主机在
  HelloAck 后排队下载，查看器接受 `FileOffer`、收集 300 KiB、校验
  Offer 摘要并回 `FileResult`；断言主机 `files_sent == 1`、字节数一致、
  无失败。可与 `--file-upload` 同时启用，覆盖两个方向并发。
- `[~]` 回环 + 本机富剪贴板人工：单机主机与查看器共用同一系统剪贴板，
  无法区分写入方；真实路径只从查看器日志确认发送过一次 DIB
  （`formats 0x8`）。浏览器 HTML/图片的双向人工核对需双机，未做。
- `[x]` `ctest -C Release` 全过；`flutter analyze`/`test`/Release 构建；
  文件传输 1 GiB 大文件不阻塞视频与输入（帧率下降 ≤10%）。

### 6.7 实施阶段

1. 协议 v16 + 单测（含增量 SHA-256）；双向化时升 v17，不新增消息。
2. QuicSession 多单向流、`CCLB`/`CFIL` 分类与队列。
3. `chast.clipboard`/`chast.transfer` 共享库与 Win32 实现。
4. 主机接入剪贴板与文件接收、设置/权限/统计。
5. 查看器原生桥、文件选择与 Flutter UI。
6. 回环探针、实机验收、文档与开发记录。

### 6.8 风险

- 剪贴板监听会自触发：写入后记录一次 hash，只能挡住紧邻的一轮；
  若用户在别处粘贴同样内容会漏发一次，可接受。
- 大 DIB（4K 约 33 MiB）会占用两端内存并产生一次集中发送，可能让
  视频短暂抖动；上限 32 MiB，必要时后续改为分块流。
- 大文件接收不能反压视频/输入：实现边收边写，应用缓冲到 32 MiB 即
  停止消费该 CFIL 流（QUIC 随之收回流控额度），降到 8 MiB 再恢复；
  查看器以 32 MiB/s 令牌桶非阻塞发送。主机轮询短暂停顿时只会压低
  文件吞吐，不会丢块或阻塞视频/控制流。
- 恶意文件名、reparse point、磁盘写满、边车伪造都按“拒绝并删除”
  处理，不尝试猜测用户意图。
- HTML/RTF 内容原样透传；查看器与主机都不渲染富文本，风险与在本地
  复制粘贴相当。
- 续传依赖边车与 `.part`；用户手动改动 `.part` 时摘要会失败并重来，
  不会把错误内容写成最终文件。

## 7. M8.5 麦克风与双向音频

目标：主机系统声音继续下行到查看器，同时把查看器麦克风上行到主机默认
扬声器，两个方向可同时工作。音频统一改为 Opus；麦克风是独立权限，默认
不启用。设备选择先覆盖查看器采集端（通信角色默认设备或指定设备），
主机播放仍跟随系统默认输出。AEC、噪声抑制与回声消除不在本轮。

### 7.1 协议 v17 → v18

版本从 17 升到 18；ALPN 不变。新增能力都沿用“独立单向流 + 首字节魔数”
的既有模型，不放宽控制流载荷上限。

| 名称 | 值 | 含义 |
| --- | --- | --- |
| `AudioCodec::Opus` | `2` | 原始 Opus 包，无 Ogg 封装 |
| `kPermissionMicrophone` | `0x0010` | 允许查看器打开麦克风上行流 |
| `kServerInfoMicrophoneEnabled` | `0x0010` | HelloAck flags 中对应的能力位 |
| `CMIC` | 4 字节魔数 | 查看器发起的麦克风单向流 |

- `kPermissionAll` 增加麦克风位；HelloAck/StreamInfo 的权限掩码与
  `PermissionUpdate` 自动携带它。旧位保持不变。
- `ServerInfo` 不再新增线上字段：麦克风能力由权限掩码推导，解码后写入
  `ServerInfo::microphone_enabled`，编码时由该布尔值回填掩码。这样
  HelloAck 与 StreamInfo 的布局只多一个 flags 位，不改变固定字段顺序。
- `AudioInfo` 继续描述音频格式；系统声音仍由主机在 HelloAck 的 audio
  块里公布 Opus 参数，麦克风格式则由 `CMIC` 流的头部自描述。
- `kMaxAudioExtradata` 保持 64 字节；Opus 的 `OpusHead` 为 19 字节。

`CMIC` 流头部固定 20 字节，后跟 `extradata_size` 字节：

| 偏移 | 字段 | 类型 |
| --- | --- | --- |
| 0 | `"CMIC"` | 4 字节 |
| 4 | 版本 = 1 | u8 |
| 5 | codec（Opus） | u8 |
| 6 | 保留 = 0 | u16 |
| 8 | sample_rate | u32 |
| 12 | channels | u16 |
| 14 | frame_samples | u16 |
| 16 | extradata_size | u16 |
| 18 | 保留 = 0 | u16 |
| 20 | extradata | `extradata_size` 字节 |

头部之后是连续的 `AudioHeader + payload`，与 `CAUD` 完全相同的打包规则；
`AudioHeader` 的 `capture_ms` 是查看器 steady clock，主机只把它当相对
时间轴，不要求与主机时钟同源。流的打开/关闭就是麦克风的开启/关闭：
没有额外的控制消息，也不存在“已授权但流未建立”的中间态。

### 7.2 编解码

Opus 直接使用 vcpkg 安装的独立 `opus.lib`（`Opus::opus`），编码/解码调用
`opus_encode`/`opus_decode`，**不经 FFmpeg 的 libopus 封装**。FFmpeg 只
继续承担 AAC 解码与 libswresample；`chast.ffmpeg` 因编解码器注册表仍需
链接 `Opus::opus`，但那只是链接依赖，Opus 数据路径不经过它。

- 系统声音：48 kHz 立体声，20 ms（960 样本），128 kbps，`OPUS_APPLICATION_AUDIO`。
- 麦克风：48 kHz 单声道，20 ms（960 样本），32 kbps，`OPUS_APPLICATION_VOIP`。
- 编码器通过 `opus_encoder_ctl(OPUS_GET_LOOKAHEAD)` 取得 pre-skip，写出
  标准 19 字节 `OpusHead` 作为 extradata；解码器从中读取 pre-skip，首包
  丢弃这段预热样本，之后的包按 `AudioHeader` 时间戳调度。
- 时间轴沿用现有规则：编码器内部按 `capture_ms` 给每个包打上首样本的
  主机/查看器 capture 时间，超过 100 ms 的空隙会把残帧补静音后重锚。
- **编码端不再保留 AAC**：`create_audio_encoder()` 只接受
  `AudioCodec::Opus`，对 AAC 返回 `Unsupported` 并说明音频统一走 Opus；
  AAC 编码器实现与工厂已删除，`AudioEncoderConfig` 的默认 codec 也是
  Opus。`AudioCodec::AAC = 1` 仅为线上兼容保留，解码器仍按
  `AudioInfo.codec` 分派，未知 codec 明确失败。
- AAC 的解码预热固定为 1024 样本；Opus 的 pre-skip 由解码器从
  `OpusHead` 读取，FFmpeg 的解码路径不参与 Opus。

### 7.3 采集与播放双工

| 端 | 下行（主机 → 查看器） | 上行（查看器 → 主机） |
| --- | --- | --- |
| 主机 | WASAPI loopback 采集 + Opus 编码 | Opus 解码 + WASAPI 默认输出播放 |
| 查看器 | Opus 解码 + WASAPI 默认输出播放 | WASAPI 麦克风采集 + Opus 编码 |

- 主机的 loopback 采集与查看器的麦克风采集各自独占线程，格式转换由
  libswresample 完成；会话线程只负责把已编码包交给传输层，不阻塞采集。
- 麦克风采集使用 `eCapture`、共享模式、事件回调。未指定设备时优先
  `eCommunications` 角色（通话音设备），没有则退回 `eConsole`；指定
  设备时按 endpoint id 打开，设备消失后每秒重试同一 id。
- 查看器麦克风默认关闭，必须同时满足：主机授予 `kPermissionMicrophone`、
  用户在 UI 打开、设备成功打开。任一条不满足都不建立 `CMIC` 流。
- 主机在收到 `CMIC` 头部后创建 Opus 解码器与播放器；流结束、权限被
  收回、解码/播放失败或会话结束时停止并释放。播放器沿用现有 80 ms
  lead 与 500 ms 队列上限。
- 麦克风播放采用“到达时间锚定”：首个包把 `capture_ms` 对齐到本机
  now + lead，不依赖 `LatencyReply`；队列因停顿被清空后下一包重新锚定，
  避免一次长时间停顿让后续音频永久判为过期。系统声音仍使用
  `LatencyReply` 的主机时钟偏移。
- 权限收回：主机 `set_permissions()` 去掉麦克风位时立即 abort `CMIC`，
  查看器在下一轮看到掩码变化后停止采集并关闭本地流；之后重新授权不会
  复用旧流，需要重新打开。

### 7.4 帧循环位置评估

M8.1 记录过“同步重建采集/编码器约 1.5 s，会丢几包音频”。本轮评估结论
是**暂不把音频发送移出帧循环**：`QuicSession` 的所有公开方法按单线程
所有权设计，音视频共用发送队列；把音频放到独立线程需要给发送路径加锁
并重新定义队列顺序，风险高于收益。麦克风沿用同一模型：

- 采集/编码在各自线程，帧循环只做 `take_packet` + `send_microphone`，
  队满返回 `WaitTimeout` 并丢当前包，绝不阻塞视频。
- 1.5 s 的同步重建仍会让麦克风短暂断流；播放器清空过期队列后按新包
  重锚，恢复为一段静音而不是永久延迟。
- 若后续要让重建期间也连续，单独开“音频 I/O 线程 + 传输层并发发送”
  的改造，需要独立的协议/线程安全评审。

### 7.5 主机接入

- `HostSettings::allow_microphone`，默认 `false`（隐私优先）。解析/保存
  与现有布尔项一致；关闭时默认权限掩码不含麦克风位，传输层拒绝 `CMIC`。
- 审批窗口新增“麦克风”复选框，只能从认证阶段的上限里做减法；会话中
  的权限窗口可实时收回。
- 主界面、托盘菜单与“安全设置 → 新连接的默认权限”都提供“接收查看器
  麦克风”开关；监听器的麦克风门禁在创建时固定，运行中从关闭改为开启
  要等下一次连接，托盘会明确提示。日志明确记录授权、上行包数、解码帧数
  与停止原因。
- `HostServerStats` 新增 `microphone_active/packets/bytes/errors/
  frames/underruns/streams`，`HostStatus` 经现有状态快照暴露给托盘。
- 麦克风播放器可通过 `HostServerConfig` 注入，回环探针用一个内存
  捕获播放器验证解码内容，真实路径仍用 `create_audio_player()`。

### 7.6 查看器接入

- `ViewerEngine` 新增麦克风状态：是否被授权、是否正在发送、当前设备、
  已发包/字节/错误/丢弃，以及最近的错误文本。
- UI 线程只设置“想开/想关”和所选设备；会话线程负责创建设备、打开
  `CMIC`、 drains 编码包并发送。切换设备等同于停止后按新 id 重开。
- 原生桥新增 `setMicrophone(bool)`、`listMicrophones`、
  `selectMicrophone(id)` 与状态序列化；Flutter 会话工具栏新增麦克风
  按钮，长按/菜单列出活动采集设备，未授权时置灰并说明原因。
- 会话结束、权限收回、设备打开失败、传输持续拒绝都会把错误写回状态，
  UI 用 SnackBar/提示展示，不影响视频与系统声音。

### 7.7 验收标准

- `[x]` `chast-protocol-test`：`CMIC` 头部往返；截断、版本/codec/
  采样率/声道/extradata 超限、保留位非零被拒绝；麦克风权限位与
  HelloAck flags 往返。
- `[x]` `chast-audio-codec-test`：Opus 立体声与单声道往返（能量、频率、
  时间戳、帧长 960），未知 codec 拒绝，以及“AAC 编码不可用、错误文案
  指向 Opus”的断言；AAC 解码仍保留缺 AudioSpecificConfig 的拒绝用例。
  测试不再依赖 FFmpeg 的 AAC 编码器。
- `[x]` `chast-quic verify --microphone`：主机授予麦克风，查看器用合成
  麦克风源发送 440 Hz 单声道 Opus；主机端注入的内存播放器断言收到
  包数、解码帧数、音调频率与 RMS，`stop_microphone()` 后主机流结束；
  再以未授权配置连接，断言 `CMIC` 被拒绝且视频会话不受影响。
- `[x]` `ctest -C Release` 11/11 全过；普通回环（系统声音改 Opus 后）
  继续通过；`flutter analyze` 无问题、`flutter test` 68 项通过、
  `flutter build windows --release` 成功。
- `[ ]` 单机人工：真实麦克风开启后主机扬声器能听到；关闭/收回权限后
  立即停止。单机扬声器与麦克风同处一室会有回声，需耳机复核。
- `[!]` 双机与真实设备回声/延迟的主观验收需第二台机器；本轮只做
  单机回环与合成源。

### 7.8 风险

- **回声**：主机播放查看器麦克风时，loopback 会把这段声音再次采进
  下行流，查看器听到自己的延迟声音；单机测试更明显。本轮不做 AEC，
  文档建议耳机，后续单独评估 WebRTC AEC/噪声抑制。
- **Opus 切换是破坏性协议变化**：v17 与 v18 不能互通，版本号已提升；
  回环、GUI 与命令行工具必须同版本构建。
- **设备热切换**：麦克风被拔出或默认通信设备改变时，采集线程按 1 s
  重试；UI 可能短暂显示“恢复中”，不会结束会话。
- **时钟锚定**：麦克风以到达时间锚定，不保证与主机系统声音相位一致；
  语音场景不需要唇音同步，若未来要做双向对齐需把查看者时钟偏移带回。
- **权限模型**：麦克风默认关闭，审批只能减少权限；但一旦授予，查看器
  可以持续发送直到关闭或收回，主机端没有逐包确认，符合现有音频模型。
- **帧循环停顿**：与 M8.1 相同，重建期间会短暂断流；已通过播放器重锚
  限制为一段静音。

## 8. M8.6 锁屏/UAC 与隐私模式

目标：锁屏或 UAC 出现时输入桌面切到安全桌面，主机不能因此断流；能打开
安全桌面的进程继续采集和注入，打不开的进程明确暂停并在解锁后恢复。
隐私模式让主机本地屏幕被遮挡，远端继续看到桌面。M8.2 已暂缓，这里不引入
虚拟显示器，遮挡只覆盖真实输出。

### 8.1 协议 v19

版本从 18 升到 19；ALPN 不变。新增权限位与三条控制消息：

| 名称 | 值 | 含义 |
| --- | --- | --- |
| `kPermissionPrivacyMode` | `0x0020` | 会话内允许 `SetPrivacyMode` |
| `kServerInfoPrivacyModeEnabled` | `0x0020` | HelloAck/StreamInfo flags 的能力位 |

| 类型 | 方向 | 载荷 |
| --- | --- | --- |
| `SetPrivacyMode = 25` | 查看器 → 主机 | `u8 enabled` |
| `PrivacyModeState = 26` | 主机 → 查看器 | `u8 enabled`、`u8 code`、`u16 reason_len`、UTF-8 reason（≤256 B） |
| `DesktopState = 27` | 主机 → 查看器 | `u8 kind` |

`PrivacyModeCode`：0 Ok、1 Denied、2 Unsupported、3 Failed。`DesktopKind`：
0 Default（用户桌面）、1 Secure（锁屏/UAC/凭据提示）、2 Unavailable
（无法查询，例如托盘主机打不开 Winlogon）。`PrivacyModeState` 的
`enabled=1` 只与 `Ok` 同时出现；其余 code 都表示遮罩处于关闭。

`ServerInfo::privacy_mode_enabled` 由权限掩码推导，不增加 HelloAck 固定
字段；权限位仍走现有 `PermissionUpdate`。重连后隐私状态总是从“关”开始，
实际遮罩状态由 `PrivacyModeState` 单独携带。`DesktopState` 在变化时发送，
HelloAck 后也发一次（即使当前是 Default），会话从锁屏开始也能立即显示
横幅。

### 8.2 输入桌面跟随

新增 `include/chast/desktop.h` + `src/win/desktop.cc`：

- `input_desktop_status()`：`OpenInputDesktop` + `UOI_NAME`，并用
  `WTSQuerySessionInformationW(WTSSessionInfoEx)` 读取锁屏标志；桌面名不是
  `Default` 即判为安全桌面，查询失败且已锁屏也按安全桌面处理。
- `follow_input_desktop()`：必要时 `SetThreadDesktop` 到输入桌面，请求
  `DESKTOP_SWITCHDESKTOP | GENERIC_WRITE`；只在真正切换时返回 `switched`，
  调用者据此判断采集对象已失效。线程不能拥有窗口或钩子。
- 主机帧循环每 1 s 检查一次：先发 `DesktopState`，再跟随桌面；发生切换
  或从暂停恢复时 `reinitialize()` 采集器、丢弃编码器、强制关键帧并重置
  输入重复帧与光标状态。
- 跟随失败（托盘主机打不开 Winlogon）时只暂停采集并每秒重试，不结束
  会话；解锁后同一条检查路径恢复采集。
- `InputInjector` 每 250 ms 跟随一次，`SendInput` 返回 0 时提前重查；
  真正切换桌面后清空按键/按钮持有状态与锁键基线，等查看器下一次修饰键
  位图重新按下仍按住的键。
- 只有以 SYSTEM 在用户会话内运行的进程能打开安全桌面：服务态工作进程
  可继续采集与注入（未实机验证）；托盘主机报 `Unavailable`，靠暂停/恢复
  保证会话存活。

### 8.3 主机隐私模式

- `HostSettings::allow_privacy_mode` 默认 `false`；`privacy_mode=` 持久化，
  解析失败保留关闭；权限掩码与 HelloAck/StreamInfo 的
  `privacy_mode_enabled` 自动联动。
- 托盘菜单与安全设置页均可开关。监听器的能力上限在创建时固定：从关闭
  改为开启需要下一次连接（托盘会提示），从开启改为关闭实时生效。
- `PrivacyOverlay`（`include/chast/privacy.h`、`src/win/privacy.cc`）：
  - 独立窗口线程先显式挂到 `Default` 桌面（即使主机线程已跟随到 Winlogon）；
  - 顶层 `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
    WS_EX_TOOLWINDOW` 黑色窗口，覆盖整个虚拟屏幕，不抢焦点、不进
    Alt+Tab、不拦截本地输入；
  - `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` 让 DXGI 采集看不到
    遮罩；系统低于 Windows 10 2004（19041）或 affinity 失败时 `create()`
    直接拒绝，绝不显示一个会把远端也涂黑的遮罩；
  - 1 s 定时器与 `WM_DISPLAYCHANGE` 重新铺满虚拟屏幕，覆盖后插入的
    顶层窗口。
- 每个会话一个遮罩。开启成功回 `PrivacyModeState(true, Ok)`；重复开启回
  当前状态；不支持回 `Unsupported`；创建/显示失败回 `Failed` 并带系统
  错误。关闭、权限收回或会话结束都会撤罩；权限收回还会主动发一条
  `PrivacyModeState(false, Denied, …)`，避免查看器显示过期状态。
- `HostServerStats` 增加 `privacy_mode_active` 与 `privacy_mode_changes`。

### 8.4 查看器

- `ViewerStatus` 增加 `privacySupported`（权限位）、`privacyEnabled`、
  `privacyCode`、`privacyError`、`privacyUpdates`、`desktop`、
  `desktopUpdates`；`acceptsPrivacy = 串流中 && 权限位`，
  `secureDesktop = Secure || Unavailable`。
- `ViewerEngine` 用“最新值覆盖”的单槽保存开关请求，由会话线程调用
  `QuicSession::set_privacy_mode()`；接收循环在取帧前后都 drain 控制消息，
  保证开关应答不被视频帧延迟。
- 原生桥新增 `setPrivacyMode(bool)` 与状态序列化；Flutter 工具栏新增盾牌
  按钮（无权限置灰），应答到达时用 SnackBar 显示成功/拒绝原因。
- 安全桌面期间没有新帧时，会话页显示“主机已进入安全桌面（锁屏或 UAC）”
  横幅；`Unavailable` 提示无法采集，恢复后自动继续。右下角状态徽标显示
  隐私模式是否开启。
- `app/test/privacy_status_test.dart` 覆盖字段解析、默认值、未知桌面码
  回退、非串流不接受隐私开关、`Unavailable` 计入安全桌面。

### 8.5 验收标准

- `[x]` `chast-protocol-test`：三条消息往返；非法 enabled 值、尾随/空载荷、
  未知 code、`enabled=1` 搭配失败 code、超长 reason、坏 UTF-8 均被拒绝；
  HelloAck 的隐私标志与权限掩码往返。
- `[x]` `chast-quic verify --privacy`（端口 45499，6 s）：主机在 1.32 s
  开启遮罩（code 0），3.54 s 关闭（code 0）；主机统计 2 次变化、结束时
  遮罩已撤下；遮罩期间采集持续、画面不是黑色（luma 与开启前差值 ≤30），
  关闭后视频继续；查看器收到 Default 桌面公告。另一台默认关闭隐私的
  主机拒绝请求且不断流。100 项断言、181 个访问单元/181 帧解码、摘要
  `4DB1C6A97132DC4F`。
- `[x]` `chast-desktop-test`：普通会话报告 Default、桌面名可读、线程已在
  输入桌面时 `follow_input_desktop` 返回 no-op；安全桌面本身无法在自动化
  会话里构造。
- `[x]` `ctest -C Release` 12/12（31.92 s）；`quic_loopback` 已加入
  `--privacy`，每次回归都会覆盖遮罩开关与视频连续性。
- `[x]` `flutter analyze` 无问题、`flutter test` 73 项全过、
  `flutter build windows --release` 成功。
- `[~]` 实机人工：真实锁屏/UAC 下的安全桌面采集与输入、托盘态暂停/恢复、
  服务态 Winlogon 采集；遮罩在多显示器与不同 DPI 下的视觉覆盖。本机只有
  1 个输出，无法自动合成键盘/鼠标与锁屏事件，以上均待实机。
- 虚拟显示器不参与本轮验收；隐私模式不改变采集分辨率或编码参数。

### 8.6 风险

- `WDA_EXCLUDEFROMCAPTURE` 需要 19041+；旧系统直接回 `Unsupported`，
  用户可以继续串流但没有隐私模式。
- 遮罩只存在于 Default 桌面。锁屏/UAC 时本地屏幕本来就是安全桌面，
  遮罩不可见；若服务态继续采集安全桌面，远端会看到该桌面，隐私模式
  不会遮盖它。需要同时隐藏安全桌面时应关闭采集或断开。
- 托盘主机在安全桌面期间暂停采集，查看器停留在最后一帧并显示横幅；
  会话不断，但输入事件会被系统丢弃（计入 failed），解锁后恢复。
- `SetThreadDesktop` 要求线程没有窗口/钩子；主机的采集与注入都在专用
  线程上，若未来把窗口逻辑移入这些线程会失败并保持旧桌面。
- 顶层遮罩可能与其他顶层窗口竞争 Z 序；1 s 定时器会重新置顶。它从不
  激活、不接管鼠标，本地操作不受影响。
- 监听器的隐私能力上限固定；运行中从关闭改为开启要等下一次连接。
- 安全桌面路径依赖服务态进程的权限与桌面访问掩码，回环无法覆盖，
  仍以实机服务安装后的验收为准。

## 9. M8.3 编码后端（FFmpeg 硬件兜底）

### 9.1 后端与选择

- `EncoderBackend { Auto, Vpl, Ffmpeg }` 经 `EncoderConfig::backend` 与
  `HostServerConfig::encoder_backend` 传递；`chast-encode` 与
  `chast-quic serve/verify` 新增 `--encoder auto|vpl|ffmpeg`。
- Auto 保持既有 Intel 路径：先 oneVPL（VDENC HEVC → VDENC AVC → VME AVC
  三级试编），全部失败后才创建 FFmpeg 编码器；固定后端失败不回退，便于
  定位。`has_encoder_available()` 与工具共用同一套判定。
- FFmpeg 后端按采集适配器厂商选编码器：NVIDIA `h264_nvenc`/`hevc_nvenc`、
  AMD `h264_amf`/`hevc_amf`、Intel `h264_qsv`/`hevc_qsv`；HEVC 不可用时
  同一厂商退到 H.264，`codec()` 报告实际格式。工程仍不含任何软件编码器。

### 9.2 零拷贝路径

- NVENC/AMF：编码器自有 D3D11VA 帧池（`initial_pool_size=1`，
  `BindFlags=D3D11_BIND_RENDER_TARGET`），采集 NV12 纹理用
  `CopySubresourceRegion` 拷进池纹理，再以 `AV_PIX_FMT_D3D11` 送入
  libavcodec；两端都在显存，只有压缩包回到 CPU。
- QSV：从同一 D3D11 设备派生 QSV 设备
  （`av_hwdevice_ctx_create_derived`），池帧映射一次得到可写 D3D11
  纹理，之后每帧同卡 GPU 拷入。派生前必须对采集设备开启
  `ID3D10Multithread`，否则 mfx-gen 的 `MFXVideoCORE_SetHandle`
  返回 `MFX_ERR_DEVICE_FAILED`（-16）。
- `shared_import()` 对 FFmpeg 后端恒为 false：这是“GPU 拷入编码器池”，
  不是共享导入；工具输出 `import: copy (GPU to GPU, no system memory)`。

### 9.3 参数与码率

- 对齐 RustDesk 的 VRAM 编码：CBR、无 B 帧、GOP 无限（仅按需 IDR）、
  单切片、低延迟、BT.601 limited、H.264 High / HEVC Main；码率缺省用
  `hardware_bitrate()`。
- `set_rate()`：NVENC/AMF 原地更新 `bit_rate`/`rc_max_rate`，纯码率变化
  继续编码序列；帧率变化延迟到下一帧前重建编码器并要求 IDR。QSV 的
  libavcodec 在码率或帧率变化时刷新并 `MFXVideoENCODE_Reset` 会话；
  oneVPL 规定不带 `mfxExtEncoderResetOption` 的 reset 会开始新编码序列
  （含 IDR），所以只有这条 FFmpeg 兜底路径每次速率变化补一次 IDR，主机
  按 `keyframe_required` 决定是否强制。
- RustDesk 对照：它的 QSV 路径同样会插入 IDR——原生 mfx 的
  `mfx_set_bitrate()` 也是不带 reset option 的 `MFXVideoENCODE_Reset`，
  `ffmpeg_vram` 只是写 `c->bit_rate` 后交给 libavcodec 做同一个 reset；
  它的 NVENC `Reconfigure`/AMF `SetProperty` 不打断序列，与我们的原地
  更新一致。RustDesk 不把自适应帧率写进编码器（mfx 的
  `mfx_set_framerate` 返回“不支持”，Rust 侧只暴露 `set_bitrate`），
  只改采集节奏；我们把 ABR 帧率同步给编码器，因此跨帧率档位会多一次
  IDR（档内纯码率变化仍是连续序列）。
- `encode_repeat()` 在内容未更新时复用池中上一帧，不重复拷贝；设备丢失
  /复位返回 DeviceRemoved/DeviceReset，由上层重建。

### 9.4 验收

- 本机（Intel Iris Xe、mfx-gen 与 FFmpeg 8.1.1）：
  `chast-encode --encoder ffmpeg --codec h264/hevc` 均输出 1920×1080、
  52.6/48.7 fps、292/256 KB（3 s），`import: copy`。
- 新增 CTest：`quic_loopback_ffmpeg`（端口 45463，含关键帧请求、文件
  上传/下载、隐私）与 `quic_stream_options_ffmpeg`（端口 45473），后者
  验证 H.264→HEVC 切换、15 fps 上限、纯码率变化（QSV 预期一次 IDR）
  与恢复。两条均通过，原有 VPL `quic_loopback`/`quic_stream_options`
  无回归。
- 回放切分修正：QSV 每帧输出 SEI+slice、无 AUD，`split_access_units()`
  改为无 AUD 时按 VCL NAL 边界成组，保存流仍与发送访问单元一一对应。

### 9.5 限制

- 本机没有 NVIDIA/AMD 显卡：NVENC/AMF 只验证了编译、厂商映射与负向
  可用性，真实编码、驱动差异与零拷贝行为待实机。
- 不改线协议（仍 v23）；编码器实现名只用于日志与 StreamInfo 展示。

## 10. 开发记录

### 2026-10-23

- `[x]` 完成与 RustDesk `c7f212d` 的发射端代码对照，形成第 2 节差距清单。
- `[x]` 建立本计划文档与状态标记约定。
- `[x]` M8.0 协议 v14：`SwitchDisplay` / `DisplaySwitchResult` 编解码与单测，
  reason 按 UTF-8 字符边界截断到 256 B。
- `[x]` M8.0 QuicSession：服务端单槽待处理请求、结果发送；客户端请求、
  结果队列与计数。主机线程在帧循环开头取请求。
- `[x]` M8.0 HostServer：热插拔后重新枚举、no-op 不再重建、先建后换、
  3 s 切屏超时、保留 ABR 码率/帧率与分辨率档位、成功后两个 IDR 并重置
  光标/重复帧状态；失败保留旧流。
- `[x]` M8.0 查看器：原生 `SwitchDisplay`、状态计数与 Flutter 会话内切换；
  失败 SnackBar 与工具栏回退。
- `[x]` M8.0 回环：`chast-quic verify` 新增当前输出 `Ok` 与越界
  `NotFound` 两条真实控制消息路径；回环 151 个访问单元、151 帧解码，
  摘要 `1BAB2DBA1362C101`，全部 151 项检查通过。
- `[x]` M8.0 构建/测试：`ctest -C Release` 10/10；`flutter analyze`
  无问题；`flutter test` 63 项通过；`flutter build windows --release`
  成功（`app/build/windows/x64/runner/Release/chast_viewer.exe`）。
- `[~]` M8.0 双屏人工验收：本机仅 1 个输出，跨输出切换未验证。
- 验证备注：连续用相邻端口跑 `verify` 时，其安全探针端口会互相占用，
  出现与本功能无关的偶发 FAIL；换独立端口段（46010）复跑后全部通过。
  探针端口布局后续可收敛，本节结论以独立端口段为准。
- `[!]` 独立 `build/Release/chast-host.exe` 未重新链接：旧进程 PID 4764
  仍占用文件。宿主库、测试与 Flutter 应用内的 host 构建均已更新；关闭该
  进程后重跑 CMake 构建即可完成链接。
- `[x]` M8.1 开工：确定协议 v15 四条消息、桌面空间模式列表、临时模式
  切换与“仅恢复到仍是我们设置的模式”的回滚语义（第 5 节）。
- `[x]` M8.1 协议 v15：四条控制消息编解码与单测（截断、超模式数、未知
  code、保留 flags、超长 reason、多个 current 标记均被拒绝）。
- `[x]` M8.1 采集层：`display_mode.h/.cc` 的模式枚举/去重/排序、当前标记、
  临时 `ChangeDisplaySettingsExW`、驱动实际生效模式与保守回滚。
- `[x]` M8.1 QuicSession：服务端单槽列表/设置请求，客户端请求与结果队列；
  列表不要求权限，设置要求 `kPermissionInput`。
- `[x]` M8.1 HostServer：权限/1 s 限流/仅当前输出/模式存在性校验；保存、
  应用、同步重建采集与编码器、保留 ABR 码率与帧率、StreamInfo、双 IDR、
  光标重置；任一步失败回滚原模式并让帧循环重建；RAII 守卫在会话退出时
  恢复。
- `[x]` M8.1 查看器：`RequestDisplayModes`/`SetDisplayMode` 原生桥、状态
  计数与失败原因；Flutter 工具栏分辨率菜单（当前模式打勾、无输入权限
  置灰、失败 SnackBar）。
- `[x]` M8.1 回环 `verify --display-mode`（端口 46090）：主机列出 84 个
  模式；1920×1080@59 → 1600×1200@59（75% 档编码 1200×900）；`StreamInfo`
  源尺寸先变到 1600×1200，恢复请求后回到 1920×1080；9999×9999 被
  `Failed` 拒绝并带原因；随后再切到 1600×1200 直接结束会话，主机在
  20.5 s 的退出守卫里自动恢复 1920×1080，探针在会话结束后直接读桌面
  模式确认。601 个访问单元/601 帧解码，序号缺口 0、CRC 错误 0、解码
  错误 0，摘要 `2DB7336853AD049E`。
- 回环发现并修复两个主机缺陷：
  1. 模式切换后驱动短时间内让 `DuplicateOutput` 返回
     `E_ACCESSDENIED`，原实现首次失败即回滚；改为在 3 s 窗口内按
     100 ms 重试。
  2. 成功路径沿用了应用模式**之前**的 `enumerate_displays()` 结果，
     `StreamInfo` 里的显示器 rect 仍是旧尺寸；改为应用后重新枚举。
- 验证备注：模式重建期间主机帧循环停顿约 1.5 s，音频队列丢 4 包
  （≤8 包断言通过）、音频错误 0，线上无音频包丢失/乱序；该停顿是同步
  重建的固有代价，留待 M8.5 评估是否把音频发送移出帧循环。
- `[x]` M8.1 构建/测试：`ctest -C Release` 10/10；普通回环 151 个访问
  单元、摘要 `E094B3206A047930` 通过；`flutter analyze` 无问题、
  `flutter test` 65 项通过、Windows Release 构建成功。
- `[~]` 查看器菜单人工点选与指针视觉复核未做；本机单输出 1920×1080，
  双屏与本项都需要实机复核。
- `[!]` 独立 `build/Release/chast-host.exe` 仍被 PID 4764 占用而未重新
  链接；宿主库、`chast-quic`、测试与 Flutter 应用内的 host 均已更新。
- `[!]` M8.2 暂缓：IddCx 驱动需要 WDK、测试签名与发布签名，开源实现
  同样绕不开；本机未安装 WDK、开启测试签名需重启。等 M8.7 的安装器与
  签名链路具备后再评估，本轮不做驱动。
- `[x]` M8.4 方案落稿（第 6 节）：协议 v16、`CCLB`/`CFIL` 单向流、
  `FileOffer/Answer/Result/Cancel`、`kPermissionFileTransfer`、共享
  `chast.clipboard`/`chast.transfer`、`.part` 续传与摘要、查看器文件
  选择与 Flutter 进度 UI。
- `[x]` M8.4 第 1 阶段：协议 v16（四条文件消息、`kPermissionFileTransfer`、
  `CCLB`/`CFIL` 头）与单测；`Sha256` 增量类。
- `[x]` M8.4 第 2 阶段：QuicSession 多单向流分类（`CCLB`/`CFIL`）与
  剪贴板/文件队列；`offer_file`/`send_file_data`/`finish_file_upload`
  等 API。发送侧新增 `try_send_file_data()`：队满返回 `WaitTimeout`，
  查看器保留该块到下一轮，视频/输入不被文件发送阻塞。
- `[x]` M8.4 第 3 阶段：`chast.clipboard`/`chast.transfer` 静态库；
  Win32 富剪贴板读写、`.part` + 边车续传、文件名清洗/唯一命名/增量
  SHA-256；单测覆盖续传、摘要失败删除与设备名/非法字符。
- `[x]` M8.4 第 4 阶段：主机接入富剪贴板与文件接收（权限、大小/空间
  校验、边收边写、完成后校验改名），设置/权限/统计/托盘进度联动。
- `[x]` M8.4 第 5 阶段：查看器原生富剪贴板轮询、`GetOpenFileNameW`
  文件选择、SHA-256 Offer、流式发送、Flutter 上传按钮与进度/错误提示。
- `[x]` M8.4 探针（`chast-quic verify --port 46141 --file-upload`）：
  103 项断言全过，181 个访问单元 / 181 帧解码，摘要
  `C72E2B6E21A96FAA`。文件部分覆盖 4 次 Offer/Answer、307200 B 新传、
  102400 B 截断续传（`resume_offset=102400`）、摘要不符拒收且 `.part`
  删除，落盘内容与 Offer 摘要一致。富剪贴板部分：四格式经 `CCLB`
  往返；关闭闸门拒绝；新增超限注入——声明总量 `40 MiB + 1` 的 CCLB
  被丢弃，会话不断，主机只写入前后两条有效内容，查看器无错误。
  该探针需要发送已编码 CCLB，为此新增 `send_clipboard_blob()`，
  `send_clipboard_content()` 复用它。畸形/超限 blob 由“断开会话”改为
  “只丢弃该流”，保留上一条剪贴板内容。
- `[x]` M8.4 构建/测试：`ctest -C Release` 11/11（35.41 s，含
  `protocol`、`file_transfer`、`quic_loopback`）；`flutter analyze` 无
  问题、`flutter test` 68 项通过；`flutter build windows --release` 成功
  （`app/build/windows/x64/runner/Release/chast_viewer.exe`）。
- `[x]` M8.4 GUI 上传：8 MiB 文件从查看器工具栏发送，主机落盘 SHA-256
  `13caf4470971ff5178359f0d94e213e9ed31fabcdce3106b6134d8d5bb5bffb5`
  与源一致。
- `[x]` M8.4 1 GiB 验收（单机回环，真实主机/查看器路径）：
  - 查看器原来的同步 `send_file_data` 在发送队列满时会卡住会话线程；
    改为 `try_send_file_data` + 32 MiB/s 令牌桶（256 KiB 突发、每轮
    8 ms 发送预算），未发出的块下一轮从 `sent` 偏移重读。修复后第 1
    次 1 GiB 传输完成，377 个可读浮层帧均 ≥27 fps。
  - 传输中启用远程输入/剪贴板后，第 3 次运行停在 789592060 B（753 MiB），
    主机日志为 `the receive queue overflowed`：主机应用缓冲旧实现只
    有 64 MiB 硬上限、无传输层背压，一次 >2 s 的主机停顿就会填满。
    改为 32 MiB 高水位/8 MiB 低水位：超水位时消费 0 字节并
    `StreamReceiveSetEnabled(FALSE)`，降到低水位后在锁外恢复接收。
  - 第 4 次运行：1 GiB 完成，落盘 SHA-256
    `49bc20df15e412a64472421e13fe86ff1c5165e18b2afccf160d4dc19fe68a14`
    与源一致；主机日志 `[16:03:55] file: received … (1073741824 bytes)`；
    进度采样稳态 19.2 MB/s（首次 58916864 B 到完成 50.4 s）。
  - 传输期间启用远程控制：查看器统计 31 fps、7.2 Mbps、`输入 9 事件`；
    500 帧浮层 OCR 得 340 个可读帧，28–32 fps（251×30、68×29、
    14×32、7×28），无低于 27 fps 的帧（目标 30 fps，下降 ≤10%）。
- `[~]` M8.4 双机人工富剪贴板：本机主机/查看器共用系统剪贴板，无法
  区分写入方；真实路径只确认真实 DIB 发送一次（查看器日志
  `formats 0x8`）。从浏览器复制含图内容、HTML 文本与反向图片的人工
  核对需要第二台机器，未做。
- `[x]` M8.4 第 6 阶段：文件传输双向化（协议 v17）。传输层去掉角色
  限制：`FileOffer/Answer/Result/Cancel` 与 `CFIL` 按发送方/接收方
  工作，权限位不变；服务端在 `announce()` 发出 HelloAck 后标记
  握手完成，主机才能发出 Offer。`HostServer` 新增
  `start_file_download()` 与帧循环下载泵（独立哈希线程、8 ms/轮、
  32 MiB/s 令牌桶、队满回退）；查看器用 `FileReceiver` 接收并暴露
  `download_*` 状态；托盘新增“发送文件到查看器…”，Flutter 显示接收
  进度与完成/失败提示。
- `[x]` M8.4 查看器拖放上传：`DragAcceptFiles` + `WM_DROPFILES` 挂在
  Flutter 视图子类窗口，首条路径经 `droppedFile` 桥接给 Dart，与
  工具栏上传共用校验/进度/SnackBar 流程。
- `[x]` M8.4 双向探针（`chast-quic verify --port 46013
  --file-upload --file-download`）：全部断言通过，181 个访问单元 /
  181 帧解码，摘要 `32B8B69C7B5EC213`。下载侧覆盖主机在 HelloAck 后
  排队、查看器接受 300 KiB、摘要一致并回 `FileResult`，主机统计
  `files_sent=1`、无失败；与上传探针并发运行互不干扰。
- `[x]` M8.4 回归：根目录 Release 构建通过；`ctest -C Release`
  11/11（33.76 s，`quic_loopback` 已加入 `--file-download`）；
  `flutter analyze` 无问题、`flutter test` 68 项通过；
  `flutter build windows --release` 成功。
- `[~]` M8.4 单机 GUI 文件往返未人工点选：托盘文件对话框、查看器
  连接/批准与拖放需要交互，回环只覆盖协议与引擎收发路径。双机文件
  往返与富剪贴板人工核对仍需实机。
- `[x]` 收尾构建已重新链接全部目标：早先 `[!]` 记录的
  `build/Release/chast-host.exe` 被占用问题已解除，当前无残留测试
  进程。

### 2026-10-24

- `[x]` M8.5 协议 v18：`AudioCodec::Opus`、`kPermissionMicrophone`、
  `kServerInfoMicrophoneEnabled`、`CMIC` 流魔数与 20 字节头部；
  `make/decode_microphone_stream_header()` 与
  `microphone_stream_header_length()` 负责先判断完整头部再解码。
  HelloAck/StreamInfo 的麦克风能力由权限掩码推导，固定字段布局不变。
- `[x]` M8.5 Opus 编解码：直接链接 vcpkg 的 `Opus::opus`，调用
  `opus_encode`/`opus_decode`，不经过 FFmpeg 的 Opus 封装。系统声音
  48 kHz 立体声 128 kbps，麦克风 48 kHz 单声道 32 kbps，20 ms/960
  样本，19 字节 `OpusHead`，pre-skip 由 `OPUS_GET_LOOKAHEAD` 写入并在
  解码首包丢弃。
- `[x]` M8.5 音频双工库：`wasapi_capture` 的系统声音改为 Opus；
  新增 `wasapi_mic.cc`（活动采集端点枚举、通信角色默认设备、设备
  消失后每秒重试）与 `opus_codec.cc`；播放器支持
  `arrival_anchored`，麦克风以到达时间锚定 80 ms lead，停顿超过
  100 ms 后重新锚定并清空旧队列。
- `[x]` M8.5 传输层 `CMIC`：`QuicServerConfig::allow_microphone`
  （默认 false）、单活动上行流、头部校验、权限门禁与实时收回；
  `receive_microphone/start_microphone/send_microphone/stop_microphone`
  与 `microphone_finished/microphone_streams` 公开 API。流关闭、会话
  重置与权限撤销都会释放句柄，迟到的旧句柄事件被忽略。
- `[x]` M8.5 主机接入：`HostSettings::allow_microphone`（默认 false，
  解析/保存 `microphone=` 与权限掩码）；审批窗口、主界面、托盘菜单与
  安全设置页均可开关；`HostServerStats` 增加麦克风计数；帧循环按需
  创建 Opus 解码器与 WASAPI 播放器，权限收回/流结束/解码失败时释放。
  `HostServerConfig::microphone_player` 供回环探针注入内存播放器。
- `[x]` M8.5 查看器接入：`ViewerEngine` 新增麦克风会被授权/正在发送/
  当前设备与名称/包数/丢弃/错误/恢复次数/最近错误，UI 线程只写“想开”
  与设备 id；会话线程负责创建设备、`start_microphone()`、每轮 drain 并
  `send_microphone()`，停止/权限收回/设备切换/失败时关闭 `CMIC` 并释放
  采集器。原生桥新增 `setMicrophone/listMicrophones/selectMicrophone`
  与状态字段；Flutter 工具栏新增麦克风按钮，长按或菜单选择输入设备，
  未授权、恢复中与错误均有说明，状态浮层显示包数与丢弃数。
- `[x]` M8.5 编码端去掉 AAC：`create_audio_encoder()` 只接受 Opus，
  对 AAC 返回 `Unsupported` 并提示音频统一走 Opus；
  `create_aac_encoder()` 与 `AacEncoder` 实现删除，
  `AudioEncoderConfig` 默认 codec 改为 Opus。AAC 解码器保留用于协议
  枚举与历史 `AudioInfo`，生产路径不再创建任何 AAC 流。
- `[x]` M8.5 阶段性回归：`chast-protocol-test`、`chast-audio-codec-test`
  与 `chast-host-settings-test` 全部通过；`chast-quic`、`chast-host`、
  `chast-service` Release 链接通过（仅既有的 LNK4098 警告）；
  `ctest -C Release` 11/11（31.63 s），普通回环在系统声音改 Opus 后
  继续通过。
- `[x]` M8.5 回环 `verify --microphone`（端口 46230，6 s）：主机授予
  麦克风，合成单声道 Opus 通过 `CMIC` 上行 90 包/7251 B，主机解码
  86088 帧（约 90×960 减去 pre-skip），注入播放器记录 440 Hz、RMS 6864；
  `stop_microphone()` 后主机流在 3.54 s 结束。关闭闸门探针的另一台
  主机拒绝 `CMIC`，同会话视频继续。179 个访问单元、179 帧解码，
  摘要 `CB960C0EAC53FDE2`，全部断言通过。
- `[x]` M8.5 测试与构建收尾：`chast-audio-codec-test` 改为 Opus-only，
  新增“AAC 编码不可用且文案指向 Opus”断言；`ctest -C Release`
  11/11（33.18 s）；`flutter analyze` 无问题、`flutter test` 68 项通过、
  `flutter build windows --release` 成功（73.7 s）。移除 AAC 编码器后
  复跑 `verify --microphone`（端口 46233）通过：`CMIC` 32 包/2592 B、
  主机解码 30408 帧、440 Hz、RMS 6895，88 个访问单元、摘要
  `EAE78CA8810C73DC`；包数随本机采集启动快慢浮动，断言只看“有包、
  全到达、可解码、音调与能量正确”。
- `[x]` 主机麦克风日志从 CMIC 头没有的 bitrate 改为打印
  `960 samples/frame`，避免出现 `0 kbps` 的误导输出。
- `[ ]` M8.5 真实麦克风人工验收：本机没有可自动合成的输入设备事件，
  需在带麦克风与耳机的机器上确认“打开即响、关闭/收回即停”；单机扬声器
  与麦克风同处一室会有回声，双机主观验收仍需第二台机器。
- `[x]` M8.6 协议 v19：`kPermissionPrivacyMode`/`kServerInfoPrivacyModeEnabled`
  （`0x0020`）、`SetPrivacyMode=25`、`PrivacyModeState=26`、
  `DesktopState=27` 与 `PrivacyModeCode`/`DesktopKind` 编解码；reason 上限
  256 B。`ServerInfo::privacy_mode_enabled` 由权限掩码推导，HelloAck 固定
  布局不变。
- `[x]` M8.6 桌面跟随：新增 `chast/desktop`（`input_desktop_status`、
  `on_input_desktop`、`follow_input_desktop`），按 `OpenInputDesktop` 与
  WTS 锁屏标志区分 Default/Secure/Unavailable；主机帧循环每秒公告
  `DesktopState` 并跟随输入桌面，切换后重建采集器、丢编码器、强制关键帧；
  托盘主机打不开 Winlogon 时暂停采集并重试，不结束会话。`InputInjector`
  每 250 ms 跟随，切换后清空按键/按钮持有状态。
- `[x]` M8.6 主机隐私模式：`HostSettings::allow_privacy_mode`（默认关）
  与权限掩码/托盘菜单/设置页联动；`PrivacyOverlay` 在独立线程挂到
  Default 桌面，建立穿透、不激活的顶层黑色窗口并用
  `WDA_EXCLUDEFROMCAPTURE` 排除采集，1 s 定时器与显示变化时重新铺满；
  低于 19041 或 affinity 失败直接拒绝。开启/关闭/权限收回/会话结束的
  状态与统计已接入。
- `[x]` M8.6 查看器：`ViewerStatus` 增加隐私与桌面字段、`acceptsPrivacy`/
  `secureDesktop`；引擎单槽排队 `setPrivacyMode`，接收循环前后 drain 控制
  消息；原始桥与 Flutter 盾牌按钮、应答 SnackBar、安全桌面横幅、状态
  徽标完成；新增 5 项 `privacy_status_test.dart` 用例。
- `[x]` M8.6 回环 `verify --privacy`（端口 45499，6 s）：100 项断言全过；
  遮罩 1.32 s 开（code 0）、3.54 s 关（code 0），主机 `changes=2` 且结束时
  `active=false`；遮罩期间采集持续、画面未变黑，关闭后视频继续；
  默认关闭隐私的主机拒绝请求且不断流。会话共 181 个访问单元/181 帧，
  摘要 `4DB1C6A97132DC4F`。
- 回环发现并修复：客户端 `permission_mask_from_info()` 只映射输入、剪贴板、
  音频、文件与麦克风，漏了隐私位，导致主机已授权时 `set_privacy_mode`
  仍回 `PermissionDenied` 并提前结束接收；补齐后全链路通过。
- `[x]` M8.6 回归：`ctest -C Release` 12/12（31.92 s）；`quic_loopback`
  已加入 `--privacy`，隐私开关与视频连续性进入常规回归；
  `flutter analyze` 无问题、`flutter test` 73 项全过、
  `flutter build windows --release` 成功。
- `[~]` M8.6 实机人工验收未做：真实锁屏/UAC 的安全桌面采集与注入、
  托盘态暂停/恢复、服务态 Winlogon 路径、遮罩在多显示器/不同 DPI/物理
  显示器上的视觉效果，都需要第二台机器或人工锁屏；本机单输出且不能
  合成系统级键盘/锁屏事件。
- `[!]` 虚拟显示器 M8.2 继续暂缓：没有可用的已签名开源驱动；隐私模式
  的“虚拟屏遮罩”按字面只落到真实输出的捕获排除浮层，不引入驱动。
- `[x]` M8.3 FFmpeg 硬件兜底：`EncoderBackend`（Auto/Vpl/Ffmpeg）进入
  `EncoderConfig`、`HostServerConfig` 与工具 `--encoder`；`create_encoder()`
  在 Auto 下先 oneVPL、失败再 FFmpeg，固定后端不回退。新增
  `src/codec/ffmpeg_encoder.cc`，按采集适配器厂商映射 NVENC/AMF/QSV 的
  H.264/HEVC，HEVC 可在同厂商退 H.264，不引用软件编码器。
- `[x]` M8.3 零拷贝：D3D11VA 帧池 + 同卡 `CopySubresourceRegion`；
  QSV 由采集设备派生并一次映射池帧。定位并修复派生失败：mfx-gen 要求
  采集设备开启 `ID3D10Multithread`，否则 SetHandle 返回 -16；独立工具
  与 `verify` 复跑均通过。
- `[x]` M8.3 速率：NVENC/AMF 原地改码率、帧率变化延迟重建；QSV 按
  libavcodec 语义在码率/帧率变化时 reset 并要求 IDR。流参数探针按
  实现区分：QSV 纯码率变化预期一次 IDR，oneVPL H.264 仍要求 0 次。
  对照 RustDesk：其 QSV（原生 mfx 与 ffmpeg_vram）同样以 reset 开始新
  序列、由硬件插入 IDR，NVENC/AMF 不插入；其自适应帧率只改采集节奏、
  不进编码器。我们的一级 oneVPL 路径用 `StartNewSequence=OFF`，纯码率
  变化不插 IDR，比 RustDesk 的 mfx 路径更细。
- `[x]` M8.3 回放切分：`tools/annexb.h` 无 AUD 时按 VCL 边界成组，
  QSV 的 SEI+slice 不再被拆成两个访问单元。
- `[x]` M8.3 验证：`chast-encode --encoder ffmpeg` H.264/HEVC 各 3 s
  通过（52.6/48.7 fps，292/256 KB）；新增 `quic_loopback_ffmpeg` 与
  `quic_stream_options_ffmpeg`，连同原有两条 VPL 回环 4/4 通过。
- `[ ]` M8.3 实机验收：NVENC/AMF 需要 NVIDIA/AMD 机器；本机只有 Intel
  Iris Xe，QSV 已完成验证。
