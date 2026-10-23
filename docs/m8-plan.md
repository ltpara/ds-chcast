# M8 — 发射端加固计划

更新时间：2026-10-23
分支：`feat/m7-security`（M7 未提交的收尾与 M8 工作在同一分支继续）
基线：M7 完成；M8.0（协议 v14）、M8.1（协议 v15）已完成；M8.4 将协议升到 v16
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
| 编码后端覆盖 | 仅 Intel oneVPL | NVENC/QSV/AMF/VAAPI | P1（M8.3） |
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
- `[ ]` **M8.3 编码后端 NVENC/AMF**：保持采集/转换/编码同卡 LUID 绑定，
  沿用现有三级硬件阶梯的试编与回退机制。
- `[~]` **M8.4 富剪贴板与文件传输**：协议 v16；HTML/RTF/DIB 与查看器
  上传、续传/摘要、GUI 1 GiB 验收已完成（见 6.6、7）。双机浏览器富
  剪贴板人工核对仍待实机；拖放与主机→查看器下载留待后续。
- `[ ]` **M8.5 麦克风与双向音频**：设备选择、Opus、采集/播放双工；
  AEC/噪声抑制单独评估。
- `[ ]` **M8.6 锁屏/UAC 与隐私模式**：服务态输入桌面切换、安全桌面采集路径、
  排除捕获/虚拟屏遮罩。
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
上传文件，支持断点续传和 SHA-256 校验。第一版不做主机→查看器下载和
资源管理器拖放，协议与 UI 预留扩展位。

### 6.1 协议 v16

版本从 15 升到 16；ALPN 不变。控制消息继续只承载协商与元数据，剪贴板
内容和文件字节走各自独立的单向流，避免 1 MiB 控制载荷上限，也避免大块
数据阻塞输入/切屏消息。

新增权限位与能力标记：

| 名称 | 值 | 含义 |
| --- | --- | --- |
| `kPermissionFileTransfer` | `0x0008` | 会话内允许向主机上传文件 |
| `kPermissionAll` | `0x000F` | 现有权限位并集 |
| `kServerInfoFileTransferEnabled` | `0x0008` | HelloAck/StreamInfo 声明可用 |
| `kServerInfoKnownFlags` | `0x000F` | 未知位仍被拒绝 |

新增控制消息：

| 类型 | 方向 | 载荷 |
| --- | --- | --- |
| `FileOffer = 21` | 查看器 → 主机 | `u32 transfer_id`、`u64 size`、`u8 flags`、32 B SHA-256、`u16 name_len`、UTF-8 name（≤512 B） |
| `FileAnswer = 22` | 主机 → 查看器 | `u32 transfer_id`、`u8 code`、`u64 resume_offset`、`u16 reason_len`、reason（≤256 B） |
| `FileResult = 23` | 主机 → 查看器 | `u32 transfer_id`、`u8 code`、`u64 received`、`u16 reason_len`、reason（≤256 B） |
| `FileCancel = 24` | 查看器 → 主机 | `u32 transfer_id`、`u8 code`、`u16 reason_len`、reason（≤256 B） |

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

文件数据流 `CFIL`（查看器 → 主机的单向流，每个传输一条）：

```
"CFIL" | u8 version=1 | u8 reserved[3] | u32 transfer_id
       | u64 offset | raw bytes ... (以 FIN 结束)
```

`offset` 必须等于 `FileAnswer.resume_offset`，字节数必须等于
`size - offset`；主机边收边写，流结束（FIN）后校验长度和 SHA-256，
再原子改名。协议单文件上限 16 GiB，主机默认再限 4 GiB。

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
- `[~]` 回环 + 本机富剪贴板人工：单机主机与查看器共用同一系统剪贴板，
  无法区分写入方；真实路径只从查看器日志确认发送过一次 DIB
  （`formats 0x8`）。浏览器 HTML/图片的双向人工核对需双机，未做。
- `[x]` `ctest -C Release` 全过；`flutter analyze`/`test`/Release 构建；
  文件传输 1 GiB 大文件不阻塞视频与输入（帧率下降 ≤10%）。

### 6.7 实施阶段

1. 协议 v16 + 单测（含增量 SHA-256）。
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

## 7. 开发记录

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
- `[x]` 收尾构建已重新链接全部目标：早先 `[!]` 记录的
  `build/Release/chast-host.exe` 被占用问题已解除，当前无残留测试
  进程。
