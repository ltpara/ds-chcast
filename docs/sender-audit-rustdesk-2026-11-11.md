# Windows 发射端完整审核与 RustDesk 功能、效果对照

审核日期：2026-11-11。本文审核当前发射端整体实现，不以 Git diff 决定范围。结论来自两侧源码、我方本轮回归和针对调度机制的新增探针；没有运行 RustDesk 二进制，不能据此宣布跨产品画质或延迟胜负。

> 2026-11-12 复核：本机 RustDesk 参照已变为 `D:/rustdesk` 的 `55786aa`（2026-11-03），本文的 `7423dce` 仍可检出但不代表当前参照，旧路径 `D:/rustdesk-ref` 已不存在。`55786aa` 已加入 Windows HDR 归一化和 `static_refresh.rs`，所以本文“参考版本没有 Windows HDR tone-map”和“参考只有 WouldBlock 重复 10 次”的描述只适用于旧基线。另：查看端出站录制已有云端上传，缺的是主机入站录制的集中归档。当前结论见 [发射端审计](sender-audit-2026-11-12.md)。

## 1. 结论

我方已经具备较完整的 Windows 远控主机能力。本次审核发现的光标采样耦合（A01）、截图控制流阻塞与流 ID 0 歧义（A03）、无设备 CPU 回退缺口（A04）、认证前连接占用正式名额（A05）已进入修复；弱网峰值帧龄门槛已收紧到 3 秒。最终实现和回归结果以 [2026-11-12 修复记录](D:/ds-chcast/docs/sender-fixes-2026-11-12.md) 为准。**A02 撤销缺陷认定：无限 P 帧、按需 IDR 是产品设计约束，录制也不引入周期 IDR。** RustDesk 的录制 GOP 策略属于设计差异，不能作为改变我方约束的理由。

共享采集编码、有界订阅队列、编码切换事务、QUIC 控制与视频分流是确实存在的机制。但“有机制”与“用户得到的效果”要分开：慢观看者仍能通过全局 QoS 降低其他人的帧率；事务协商仍会同步暂停显示服务；控制流也会被截图等自身数据占用。不能把这些概括为“慢端完全不影响快端”“切换无停顿”或“控制永不受阻”。

## 2. 基线、范围和证据等级

| 项目 | 本轮口径 |
|---|---|
| 我方 | `D:/ds-chcast`，HEAD `ca51be2` 加审核期间的工作区内容，协议 v70 |
| RustDesk | `D:/rustdesk-ref`，HEAD `7423dced37845c48093b6f71bd82e4a16cc8c79d`，提交日期 2026-08-22；本轮检查时工作区干净 |
| 源码范围 | 主机准入、Windows 服务/会话、采集/转换/编码、调度/背压/QoS、光标、音频、多屏/摄像头、截图/录制、输入、剪贴板/文件及隐私等辅助能力 |
| 接收端追踪 | 只在确认协议闭环、反馈时点和录制/截图语义时追踪，不将其表述为接收端完整审核 |
| 运行验证 | 我方 10 项相关 CTest、新增调度与策略探针；RustDesk 只做静态对照 |
| 并发变化 | 审核中发现查看端截图状态、VPL 动态码率和文档继续变化；本文没有覆盖或回退这些编辑。测试只证明对应构建时的代码。测试结束后记录源码摘要于 `build/review-sender-source-sha256.txt`，它不代表构建输入的精确快照 |

证据分三类：**本轮实测**、**源码可推出的行为**、**需要实机或跨产品 A/B 验证的效果**。文中明确区分三者。现有文档只作为线索，不直接继承其“对齐”“占优”结论。

## 3. 功能覆盖与效果矩阵

| 功能面 | 我方现状 | 本地 RustDesk 对应实现 | 效果判断与边界 |
|---|---|---|---|
| 准入、密码、人工同意、TOTP、可信设备、权限 | `host_security.cc`、`host_server.cc`；有会话票据与动态撤销 | `server/connection.rs`、`login_failure_check.rs` | 主要能力已有；已拆分传输上限和认证后正式配额，见 A05 与修复记录 |
| Windows 服务、安全桌面、会话选择、SAS | `host/service.cc`、`worker.cc`、`windows_sessions.cc`、`win/desktop.cc` | `platform/windows.rs`、`server/portable_service.rs` | 已有实现；SYSTEM 工作者与普通托盘权限不同，真实 UAC/锁屏/多用户切换效果未在本轮验收 |
| 便携提权 | `elevation_handoff.cc` 保留身份、移交配置、停止旧监听、启动提权主机；查看端重连 | `server/portable_service.rs` | 属于重连交接机制，不能等同于保留当前 QUIC 会话的无缝提权 |
| DXGI / GDI 采集及恢复 | 自动采集器可在重建失败时换后端；`gdi-cpu` 在无 D3D 设备时输出系统内存 BGRA 并由软件编码消费；GPU 线程串行持有资源 | `libs/scrap/src/dxgi/mod.rs`、`gdi.rs` | 均有回退；我方 `gdi-cpu` 不再依赖 D3D11/Nv12Pass，见 A04 补记；CPU 路径的实际帧率与画质未与参考产品同条件比较 |
| HDR、颜色、旋转 | scRGB→SDR、GPU 旋转、GPU BGRA→NV12；默认 BT.601 limited | 参考版本有旋转、GDI 与纹理/像素缓冲路径；本轮未找到 Windows HDR tone-map 实现 | 我方有明确 HDR→SDR 路径；这不等于端到端 HDR，也没有 HDR 显示器的同场景效果比较 |
| 硬件编码 | oneVPL、FFmpeg NVENC/AMF/QSV；D3D11 纹理输入 | VRAM / HWRAM 编码，具体能力受构建特性和外部 hwcodec 影响 | 都有 GPU 编码路径；不同 GPU、驱动、初始化与重置成本未做同条件比较 |
| 软件编码与格式 | 软件回退为 VP9（libvpx，4:2:0 / 4:4:4）；AV1 只有硬件路径；OpenH264 已于 2026-11-12 移除 | VP8、VP9、软件 AV1，VP9/AV1 可 4:4:4 | 软件回退格式已与参考一致（VP9）；我方仍缺 VP8、软件 AV1、AV1 4:4:4；不应把“支持更多格式”直接当成画质更好 |
| 画质与码率 | 质量比例、基础码率表、动态码率；VP9 与硬件使用的预算公式有差别问题 | `common/codec.rs`、`common/vpxcodec.rs`、hwcodec | 硬件公式相近不代表所有编码器等价；VP9 预算、量化和丢帧参数并未对齐，见 B02 |
| 静止画面刷新 | 按编码器、后端、画质设置有限重复预算；AV1 不刷新 | `video_service.rs` 在 WouldBlock 下仅对非 latency-free 编码器最多重复 10 次 | 我方覆盖更多硬编刷新场景；清晰度增益和额外功耗没有量化，不能仅按次数宣布更好 |
| 同屏共享、多人接入 | 一个 Source 多个订阅，同格式可复用编码器；格式变化先证明再提交 | 新订阅且已有观看者时触发 `SWITCH` 重建 | 我方避免了部分不必要重建；跨源协商会设置 `negotiating` 并同步等候，不能声称零停顿 |
| 背压与慢端 | 每订阅 60 帧/8 MiB；溢出清队列等 IDR；全满时继续采样、停止编码 | 无界连接通道加每帧最多 3 秒等待取帧通知 | 我方有明确队列边界；但共用 QoS 仍让慢端降低全局质量/FPS，见 B01 |
| 弱网与实时性 | RTT、探测超时、视频取帧确认驱动全局策略；可靠视频流 | TestDelay、可选 VideoReceived、逐帧取帧等待，共用编码策略 | 两者策略来源相近，但不是同一控制器；本轮修复前弱网峰值帧龄 1.738 秒，修复后 2.446 秒，不能写成低延迟已达标 |
| 光标、协作提示 | 指针更新独立发送、缓存/合并、512 px sprite、本机箭头提示 | 独立约 33 ms 的 cursor/position 服务，另有白板/姓名/彩色提示 | 我方已增加独立采样线程，低 FPS 和慢编码的修复前后数据见 A01 与修复记录 |
| 系统音频、双向麦克风、通话 | WASAPI、共享 loopback、Opus、DATAGRAM、DSP、设备恢复与通话换源 | audio service、Opus LowDelay、音频输入/voice-call 切换、静音门 | 我方工具更多，但帧长和启用位置不同，不能推导整体通话更好，见 B03 |
| 多屏、拓扑、摄像头 | 独立屏幕流与拓扑恢复；同适配器合并画布；摄像头复用流池 | 每屏/摄像头视频服务、全局编码和 QoS | 我方多合并视图，但要求屏幕在同一 GPU；合并编码有尺寸/面积约束，文字可能被缩小 |
| 主机原始截图 | 原始 BGRA 读回→PNG→分块响应，失败回退查看端画面 | 原始帧截图；VRAM 路径可能临时关闭后重建 | 我方不必为截图重建编码器，但GPU 读回仍占用显示源线程，PNG 已移到辅助线程；v72 后分片走专用 CSHT 流并限在途 512 KiB，不再占用控制流，见 A03 |
| 主机录制、查看端录制 | 压缩包直写 MP4；主机 10 分钟分段、32 MiB/120 帧队列；无限 P 帧、按需 IDR | 压缩包录制；ClientRecordStatus 驱动 240 帧关键帧间隔；有录后上传模块 | 关键帧策略是设计差异；我方录制持续期间不定时插入 IDR，见 A02 |
| 键鼠、文本、触控/笔、按键释放 | `win/input.cc`、协议和权限闸门有路径 | `input_service.rs`、enigo、平台代码 | 不是缺少基本输入；混合布局/IME、真实 UAC 和触笔仍需端到端验收 |
| 剪贴板、文件浏览/传输 | 富文本/图像、方向控制、压缩、惰性文件内容、目录、续传、冲突协商 | clipboard service、cliprdr、文件任务 | 主要能力已有；第三方 OLE 虚拟文件源读取缺失，Outlook 等“复制附件”不等同于 Explorer 复制文件 |
| 终端、端口转发、远程配置、防休眠 | ConPTY、TCP relay、配置权限、PowerRequest、壁纸恢复 | terminal service、port-forward、配置与平台服务 | 有代码闭环；本轮没有把“有处理函数”当成部署和长期运行已验收 |
| 隐私、虚拟显示、无头 | 覆盖层、虚拟显示管理、独立恢复守护、签名/路径检查；vdd provider 明确未实现 | MAG、虚拟显示及多种平台/后端管理 | 我方有恢复设计，实际可用性取决于驱动部署、GPU 和会话权限，不能宣布全后端等价 |
| 扩展能力与平台 | 当前主机仅 Windows；无远程打印、角色互换、完整白板、录制上传 | 对应服务/协议/模块存在；支持更多主机平台 | 这些是明确的产品功能差异；优先级应低于关键交互质量的闭环 |

## 4. 优先处理的实现与闭环问题

### A01：光标采样被视频 FPS 和同步编码阻塞（效果缺陷）

证据：我方 [desktop_stream.cc](D:/ds-chcast/src/host/desktop_stream.cc:687) 的同一 `pump()` 按编码器 FPS 安排捕获，捕获后 `notify_cursor()`，随后同步编码。RustDesk [input_service.rs](D:/rustdesk-ref/src/server/input_service.rs:330) 的 `new_cursor`、`new_pos` 使用约 33 ms 的独立服务。

本轮使用 `tests/host_media_stub.h` 提供确定性采集器和编码器，实际运行生产的 DesktopStreamPool 调度及指针订阅回调。计时从修改采集器的指针状态到订阅回调收到对应位置，不含网络、Windows 指针采集和查看端渲染。

| 视频 FPS | 编码器注入阻塞 | 样本数 | 平均指针等待 | 最大等待 |
|---|---|---|---|---|
| 30 | 0 ms | 8 | 33.47 ms | 34.53 ms |
| 5 | 0 ms | 8 | 179.23 ms | 200.73 ms |
| 30 | 300 ms | 1 | 304.96 ms | 304.96 ms |

低 FPS 样本在上次回调后立即更新指针，主要刻画最坏调度间隔，不是随机移动的总体延迟分布。即使如此，它已反证“采样独立于视频”。真实链路还会叠加发送和绘制时间。

建议：单独调度光标位置/形状采样，保持 GPU 资源和安全桌面切换的线程约束；不能只把 `notify_cursor` 换一个发送队列。验证 5/15/30 FPS、300 ms 编码阻塞、视频队列满和桌面切换。

**2026-11-11 补记（已修复）。** 显示源新增独立指针线程 `Source::start_cursor`
（`src/host/desktop_stream.cc`）：先尝试切入输入桌面，再约 33 ms 轮询
`sample_desktop_cursor`，只更新最新快照并回调当前订阅者的 sink；捕获 tick、
同步编码和发送队列都不参与该线程的节拍。未提供采样器的源（合成/相机等）
仍沿用采集线程回调。按上文验收口径新增 `desktop_stream` 用例（5/30 fps，
编码器注入 300 ms 阻塞）：5 fps 修改指针到回调收到 14.45/46.22 ms，30 fps
62.37/16.01 ms，均低于 150 ms 门槛，且在 5 fps 下不再出现约 200 ms 的最坏
调度间隔。视频队列满与桌面切换的实机断点、真实 Windows 指针采集耗时和
双机网络延迟仍不在该用例覆盖内。

### A02：录制关键帧策略差异（设计约束，撤销缺陷认定）

我方默认 `gop_size = 0`、`HostServerConfig::keyframe_interval_ms = 0`，按无限 P 帧、按需 IDR 工作。录制启动、新文件/分段、解码恢复等事件可以按需请求 IDR；不会因为录像器持续打开而每 8 秒或每固定帧数插入 IDR。

RustDesk 的 `ClientRecordStatus` 会更新主机录制状态，视频服务为录制设置 `Some(240)` 关键帧间隔。这是两者的设计差异。此前把缺少周期录制关键帧列为功能闭环缺失，并建议定期请求 IDR，不符合产品约束，现撤回该建议；相关主机和查看端定时请求均已移除。

长 GOP 下随机定位可能需要解码更多帧，这应作为录制体验的权衡单独测量，不能以改变编码策略的方式解决。回归覆盖静态查看端录像超过 8 秒不新增请求、主机录像在 8 秒和分段前不新增请求，同时保留分段边界的按需 IDR。

### A03：截图会影响控制实时性，且流 ID 0 有歧义（实现缺陷）

证据：我方 [host_server.cc](D:/ds-chcast/src/host/host_server.cc:5705) 循环将所有 PNG 分块入队；[quic.cc](D:/ds-chcast/src/net/quic.cc:7038) 将响应发送到同一个 `control` 流。截图上限 24 MiB，发送总队列阈值 32 MiB，因此单次截图通常全部入队。切成 128 KiB 满足消息大小限制，但不会消除同一可靠流的按序等待。

影响：8 MiB PNG 在 5 Mbps 下仅理想传输时间约 13.4 秒，后发的主机→查看端控制响应和光标更新排在 PNG 后面。反向输入不是直接排在同一发送方向后面，但权限/操作响应延迟、共享容量和会话循环阻塞仍影响交互。查看端 10 秒超时只取消本地组装，不会撤回已发数据。

另一个问题是 [host_server.cc](D:/ds-chcast/src/host/host_server.cc:5669) 将 `stream_id == 0` 解释为当前流，而最初的视频流合法 ID 也是 0。多屏保留流 0、主机聚焦其他流后，明确请求原流 0 会得到当前流截图。

读回经显示服务线程同步执行，PNG 在会话线程编码；因此“没有重建编码器”不能写成“没有暂停”。应分别度量 GPU readback、PNG 编码、排队和传输耗时。

建议：图片使用独立流并限制在途数据；分离“当前屏幕”与明确流 ID；验收大 PNG 与光标/权限变更并发。前轮复现的连续截图错配在本轮后段已看到新增 `pending_screenshot_restart_` 处理，因此不沿用为当前开放缺陷，也未将旧复现当成新实现的验证结果。

补记（2026-11-11/12，关闭）：工作区已把 `ScreenshotResponse` 分片改到专用 CSHT 单向流（`send_screenshot_response` 首次分片 `QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL`，流优先级 0），`send_bytes_impl` 为截图单独累计 `screenshot_in_flight_bytes`，超过 512 KiB 即返回 `WaitTimeout` 交由主机下一轮重试，控制流不再排在大 PNG 后面。`kScreenshotCurrentStream` 改为 `UINT32_MAX` 哨兵，0 是合法的显式流 ID；主机仅把哨兵解析为当前聚焦流。验证：`protocol`（消息往返、哨兵、边界）与 `host_control`（真实桌面 `request_screenshot(0)` 收齐 PNG）在本工作区构建通过（7.67 s）。修复后 PNG 编码移到辅助线程，会话轮询结果；残留边界：GPU 读回仍在显示源线程串行执行；24 MiB 上限下仍会占据 512 KiB 发送窗口直至完成；大图并发光标延迟未单独测量。

### A04：软件编码回退仍依赖完整 GPU 采集/转换（兼容性缺口）

证据：主机固定要求 NV12；[gdi_capturer.cc](D:/ds-chcast/src/capture/gdi_capturer.cc:61) 仍创建 D3D11 设备，上传 BGRA 并建立 Nv12Pass。Nv12Pass 要求视频处理器接口，OpenH264 再从 NV12 GPU 纹理读回 CPU。VP9 4:4:4 虽使用 BGRA，显示服务入场仍要求可用 NV12。

RustDesk GDI 返回系统内存像素，`common/convert.rs` 有 libyuv 转换，软件 VP9/AV1 可沿像素缓冲路径运行。

影响：我方能应对“硬件编码器失败，但 D3D11 采集与视频处理器仍正常”；不能据此保证视频处理器/显卡驱动不可用时也能工作。这是退化路径的结构限制，本轮未通过禁用驱动复现，不宣称某个具体 GPU 必然失败。

建议：确定产品是否要求 CPU-only 兜底；若要求，增加 GDI 系统内存→libyuv→软件编码路径，或至少明确报告失败发生在采集/转换还是编码阶段。

**2026-11-11 补记（工作区已修复，来自并发编辑，本轮仅验证）。** `CaptureBackend` 新增 `GdiCpu`：`gdi_capturer.cc` 在该后端下不创建设备、`GdiCapturer` 不建 Nv12Pass，`BitBlt` 后把 DIB 转成 `FrameInfo::cpu_bgra`；`shared_capturer.cc` 的读者在 `cpu_bgra` 时跳过共享纹理，`desktop_stream.cc` 将 CPU 像素直接交给 `OpenH264Encoder::encode_cpu_bgra` / `VpxEncoder::encode_cpu_bgra`。回退入口有三处：`AutoCapturer::initialize` 在 DXGI 失败时建 GDI，`AutoCapturer::capture` 在帧没有视频图像时切 `GdiCpu`，`Reader::fallback_cpu` 在 NV12Pass 初始化/转换失败时切 `GdiCpu`（合并采集的外置设备除外）。截图在 CPU 帧上直接读保留的 BGRA，不必为截图重建编码器。

验证：`chast-desktop-stream-test` 与 `chast-host-parity-test` 重建后通过（17.10 s、154.59 s）；其中 `cpu_encoding_without_device` 覆盖无 D3D 设备的 H.264/VP9 4:2:0/4:4:4 首帧 IDR、增量帧和按需 IDR，`cpu_capture_pipeline` 在本机单屏上真实打开 `GdiCpu` 共享采集，确认帧无 GPU 纹理/设备、`Auto` 选择软件 H.264、编码出关键帧、CPU 截图尺寸正确（2026-11-12 晚起 Auto 改选软件 VP9，软件 H.264 用例已删除）。本轮另在 `shared_capturer` 测试中补充共享读者用例（`gdi-cpu` 后端、两个读者各自缩放），已注册为 CTest `shared_capturer` 并通过（0.37 s）。边界：未在禁用显卡/驱动的机器上复现视频处理器不可用；CPU 路径的帧率、CPU 占用和多屏开销未与 RustDesk 同条件比较；合并采集（多屏同 GPU 画布）仍要求外置设备，外置设备上的 NV12 失败仍返回 `FatalError`，没有无设备回退。

### A05：认证前连接占用正式观看者名额（可用性问题）

证据：[host_server.cc](D:/ds-chcast/src/host/host_server.cc:1180) 将 `max_viewers` 直接用作 `quic.max_clients`；[quic.cc](D:/ds-chcast/src/net/quic.cc:1307) 在新连接到达、Hello/应用认证之前增加 `live_clients`，超额则关闭新连接。

影响：若上限为 N，N 个停留在连接/认证前阶段的连接可暂时排挤合法观看者。超时和身份认证仍存在，但身份认证正确不意味着名额隔离。RustDesk 也不能假设不存在资源耗尽风险；本轮只确认我方这条具体生命周期，不推导参考产品已经完全解决。

建议：分别限制 pending transport、pending auth 与 admitted session；认证后才消耗正式观看者配额，并加入建立连接但不发送 Hello 的回归。此项为源码判断，本轮未新增网络攻击压力测试。

**2026-11-11 补记（工作区已部分实现，来自并发编辑，本轮仅核对）。** `QuicServerConfig` 新增 `max_authenticated_clients`（`include/chast/quic.h:143`）：`max_clients` 继续限制全部传输层连接，`HostServer::open_session` 把它设为正式观看名额再加 8（`src/host/host_server.cc:1184-1185`，`serve_once` 时正式名额为 1）。`src/net/quic.cc:6400-6406` 在 Hello、认证和人工批准全部通过后才原子申请正式名额，超过 `max_authenticated_clients` 时以 `Declined`（"the host's viewer limit has been reached"）拒绝，并在会话关闭时归还（`quic.cc:1180`）。因此“停留在握手/认证前的连接占满正式观看名额”已不成立，未认证连接最多占 8 个额外传输槽。剩余边界：pending 预算目前是固定 +8，没有按来源（地址/中继）分配配额；未认证连接仍可在 `hello_timeout_ms`（默认 10 s）内占满这 8 个槽并短暂拒绝新传输；没有“建连不 Hello / 认证失败 / 取消批准后名额归还”的自动化回归；本轮未构造压力测试，不声称已抵御资源耗尽。相关代码由并发工作流写入，本轮只确认编译（`host_parity` 通过）与源码路径，不代为宣布验收完成。

**2026-11-12 修复补记。** 传输连接上限与认证后正式配额已拆分；正式配额在认证和人工同意后获取。正式上限为 1 时也使用独立会话工作线程，满额拒绝不提交临时密码消费和信任状态。`host_multi` 覆盖上限 1/2 时建立 TLS 但不发送 Hello 的连接，合法文件会话仍获准进入，并检查正式连接上限。总传输连接数仍有有限上限，不能把本次修复表述为消除所有资源耗尽风险。最终回归见修复记录。

## 5. 不是“缺功能”，但决定实际效果的差异

### B01：队列隔离存在，全局质量隔离不存在

[video_qos.cc](D:/ds-chcast/src/host/video_qos.cc:262) 对所有用户取最保守 FPS，码率也根据最差延迟调整；同一个策略供所有显示源使用。独立视频队列可以避免慢端的队列直接阻塞快端，但无法保证快端仍保持原始质量。

本轮直接运行生产 VideoQos：两个观看者干净反馈后 60 FPS；观看者 2 出现 5 秒未消费等待，全局变为 5 FPS；观看者 2 离开后恢复 60 FPS。这是确定性策略测量，不是两台真机帧率测量。RustDesk 同样使用共享最保守策略，这是带宽和编码成本的设计取舍，不应当作单独的实现漏洞。

新观看者兼容时无需重建编码器，已由本轮 desktop_stream/host_parity 验证覆盖。但新连接仍会触发初期 FPS 约束、请求 IDR；格式或色度变化时跨 Source 暂停并证明候选。所以准确表述应为“减少不必要的重建”，不是“接入对现有观看者没有影响”。

### B02：VP9 的同名画质预设并不等价

我方 [vpx_encoder.cc](D:/ds-chcast/src/codec/vpx_encoder.cc:35) 及 DesktopStreamPool 对 VP9 使用带非 H.264 硬件系数的码率公式；参考 [vpxcodec.rs](D:/rustdesk-ref/libs/scrap/src/common/vpxcodec.rs:314) 是 `base_bitrate × ratio`。以 1080p、Balanced 0.67、自动预算且扣除音频前为例：参考约 1388 kbps；我方约 2083 kbps，约为 1.5 倍。手工码率上限和音频预算会改变实际结果。

参考还按 ratio 改 `rc_min_quantizer/rc_max_quantizer`、允许编码器丢帧；我方固定量化区间 0–56、`rc_dropframe_thresh=0`，每个 VP9 编码器最多 8 线程。相同的 CPU-used=7 不足以证明同样的质量/带宽/功耗。

这不直接证明我方更差：更高预算可能换来更清晰图像，也可能更容易拥塞；不丢帧可能更连续，也可能加重慢 CPU 的延迟。必须在相同实际码率、内容、分辨率与延迟限制下比较。既有报告“码率/画质公式全部对齐”的表述过宽。

### B03：音频功能更多，不等于更低延迟或更好通话

我方协议 `kOpusFrameSamples=960`，48 kHz 下为 **20 ms**，Opus 使用 AUDIO 或 VOIP application。参考音频服务使用 LowDelay，常规立体声按 **10 ms** 分块。两者集包时间、编码器模式、码率和缓冲不能视为相同。

> **2026-11-12 晚修正**：本节为 2026-11-11 的记录。当前已改为 `kOpusFrameSamples=480`（10 ms）+ `OPUS_APPLICATION_RESTRICTED_LOWDELAY`，帧长与低延迟模式已与 RustDesk 对齐；码率仍显式设置（回环 128 kbps、麦克风 32 kbps），且仍是 DATAGRAM + PLC。系统音频队列深度按 25 包计约 250 ms，不再是 500 ms。

我方系统音频共享队列每订阅 25 包，20 ms 下约 500 ms；最底层采集队列、发送和播放器还会叠加等待。这是上界和结构说明，不是正常负载延迟实测。DATAGRAM 减少可靠重传导致的过期音频等待，同时需要处理丢失；其优势不能用历史整条链路的不同参数结果简单归因。

SpeexDSP 路径确实存在，本轮合成 DSP 测试中噪声地板改善约 14.27 dB、保留约 85.04% 测试音能量、立体声样本 ERLE 约 29–31 dB。但这些是合成信号结果，不是扬声器/麦克风双讲主观评分。主机 voice-call 的 `create_voice_capture()` 默认配置没有自动开启 echo/noise 标志；不能说每条通话都已启用 AEC/NS。

### B04：当前弱网验证证明可恢复，尚未证明交互时延达标

本轮重建后执行 `quic_weak_network`：真实 Intel 硬编，UDP netem，80 ms 单向基础延迟、0–20 ms 抖动、3% 丢包、1 Mbps；4–12 秒施加损伤。

| 指标 | 本轮结果 |
|---|---|
| 访问单元 | 419 发送 / 419 接收 / 419 解码 |
| 关键帧 | 8 |
| CRC、解码错误、可靠视频序号缺口 | 0 |
| 目标码率 | 配置 3000，最低 1000，恢复到 1520 kbps；12 次调整 |
| 末尾视频几何/FPS | 保持所选尺寸，30 FPS |
| 峰值帧龄 | 1738 ms |
| 末尾取帧交付/待收年龄 | 8 ms / 9 ms |

修复前测试的峰值帧龄门槛是 [tools/quic.cc](D:/ds-chcast/tools/quic.cc:10450) 的 `<15000 ms`。因此“PASS”说明符合这个宽门槛，并不能说明交互达到几百毫秒。此前文档中旧控制器的 8 秒、2.1–3.3 秒结果也不能代表当前策略。

2026-11-12 修复补记：峰值帧龄门槛已改为 `<3000 ms`。关闭 QUIC 内部发送缓冲后的最终回归为 392/392/392 访问单元发送/接收/解码，峰值 2446 ms，14 次码率调整并恢复末尾 30 FPS。它通过更严格的门槛，但数值不证明相对于旧基线有性能提升，更不能等同于几百毫秒交互体验。

视频取帧 ACK 在 `QuicSession::receive_frame()` 返回压缩帧时生成，发生在解码和显示之前。它对判断传输/消费积压有价值，但不是 capture-to-display 或 input-to-photon 测量。单机共享时钟和 netem 同样不能替代跨机真实路径。

### B05：已有画质数字只覆盖编码器级局部

现有 `encoder-quality-comparison-2026-11-11.md` 比较的是我方 VPL 与 FFmpeg QSV，约 1080p、实际 2.5 Mbps，各自以自己的 NV12 输入为参考：Y-PSNR 均值约 55–56 dB，SSIM 接近 0.999。两路是不同时间窗口，逐帧回读使采集约 28 FPS。

这些数字可支持“这两条路径在这些短桌面片段的编码损失较小”。它们不能支持“我方画质胜过 RustDesk”，也不能覆盖 BGRA→4:2:0 已丢失的彩色细字信息、4K、低于 1 Mbps、高动态、真实呈现延迟或 CPU/GPU 功耗。评价远控文字清晰度应另用原始 RGB/4:4:4 参考，并计入缩放和色度采样损失。

## 6. 明确缺少与部分覆盖的能力

| 项目 | 判断依据 | 用户可见影响 | 实施建议 |
|---|---|---|---|
| 独立光标采样 | ~~见 A01~~ 已修复：独立指针线程约 33 ms，见 A01 补记 | 低 FPS/慢编码时鼠标反馈慢 | 已关闭；实机桌面切换与双机延迟待验收 |
| 录制关键帧策略 | 见 A02：无限 P 帧、按需 IDR | 长 GOP 随机定位开销需测量 | 保持设计约束，不引入周期 GOP 或定时 IDR |
| 第三方虚拟剪贴板文件源 | 我方读取侧主要 CF_HDROP；虚拟文件实现主要是 IDataObject 发布侧；参考 cliprdr 读取 FILEDESCRIPTOR/FILECONTENTS | Explorer 文件可复制不等于邮件附件等虚拟文件可复制 | 用 Outlook/Office/压缩软件真实数据源验收 |
| 全 CPU 退化链路 | 见 A04 补记 | `gdi-cpu` 无设备采集与软件编码已接通；合并采集仍要求外置 GPU 设备 | 单屏兼容性缺口已收窄，禁用驱动场景与多屏合并路径仍需实机验收 |
| 软件 AV1、VP8、AV1 4:4:4 | 工厂/协议与参考编码配置对照 | 部分格式覆盖和无 AV1 硬件机器的选择较少 | 不必为了表格对齐强行引入；先确认受众 |
| 远程打印 | 参考 printer_service、PrinterData 协议/IPC；我方未找到对应执行链路 | 不能重定向远程打印任务 | 产品扩展 |
| 协作光标 | 我方 x/y/visible + 按下边沿；覆盖层有颜色、姓名和点击波纹，无自由标注 | 参考 whiteboard 模块 | 缺标注绘制 | 产品扩展 |
| 切换控制方向 | 参考 SwitchSidesRequest；我方无对应协议 | 不能一键互换主控/被控角色 | 产品扩展 |
| 录制后上传 | 参考 hbbs_http/record_upload；我方查看端出站录制已有云端上传/配额，主机入站录制只写本地 | 主机侧缺集中归档上传闭环 | 部署需求决定；参考也依赖服务配置 |
| 更多虚拟显示后端、其他主机平台 | 我方 vdd 明确 Unsupported，主机仅 Windows | 驱动/平台覆盖较窄 | 单独规划，不能混同 Windows 当前体验 |

## 7. 本轮验证与可复查材料

重建相关生产库及 10 个目标后，以下 CTest 全部通过，耗时 199.08 秒：`capture_transform`、`host_settings`、`host_parity`、`desktop_stream`、`display_subscriptions`、`video_qos`、`host_security`、`audio`、`audio_dsp`、`quic_weak_network`。`host_parity` 包含模拟边界及真实 QUIC；不能据测试名将所有场景都当成真实 GPU 实测。

本轮材料保存在本地 build 目录：

- [构建日志](D:/ds-chcast/build/review-sender-build.log)。
- [CTest 汇总](D:/ds-chcast/build/review-sender-tests.log) 与 [本轮完整输出快照](D:/ds-chcast/build/review-sender-ctest-details.log)。
- [调度/策略探针源码](D:/ds-chcast/build/review-sender-probe.cc) 与 [输出](D:/ds-chcast/build/review-sender-probe.log)。探针不需要移动真实鼠标、安装驱动或操作真实麦克风。
- [源码摘要](D:/ds-chcast/build/review-sender-source-sha256.txt)，用于区分审核中的继续编辑。

初始审核阶段只新增报告和验证材料。后续已按用户授权修复业务实现，见 2026-11-12 修复记录；本文的原始实测表格保留作修复前基线。没有构建或运行 RustDesk，也没有安装系统服务。

## 8. 下一批验收应围绕用户效果

A01、A03、A04、A05 与弱网门槛的修复和最终验证见修复记录。后续优先补齐双机用户效果验收；保持无限 P 帧、按需 IDR。增加编码格式属于后续独立决策。

| 场景 | 应记录的指标 | 当前缺少的证据 |
|---|---|---|
| 同内容对照 | 相同 GPU/编码格式/实际码率/分辨率；原始 RGB 与最终呈现图的误差、彩色细字可读性 | RustDesk 二进制同条件 A/B，当前没有 |
| 低 FPS 与慢编码 | 光标采样间隔和到达延迟的 P50/P95/最大值，分别 5/15/30 FPS、300 ms 编码阻塞 | 独立采样已有 5/30 FPS + 300 ms 阻塞的确定性证据（A01 补记），缺 15 FPS、真实 OS+双机完整链路 |
| 突降带宽、突发丢包、恢复 | 最大和 P95 帧龄、坏链路收敛时间、恢复时间、停格次数、输入响应时间 | 当前 netem 为短时单机；修复后门槛收紧到 3 秒，仍缺双机呈现延迟 |
| 一快一慢、多屏、多人接入 | 快端 FPS/码率下降幅度、编码重建次数、接入停顿、订阅/发送/解码队列年龄 | 确定性共享策略已有，缺跨机体验量化 |
| 原始截图并发 | GPU 读回、PNG 编码、传输时间；期间光标及权限响应延迟 | 单屏小截图成功不代表大图/多屏达标 |
| 查看端单独录像 | 拖动到随机时间的解码帧数/耗时，静止桌面和录制开关生命周期 | 已补无周期 IDR 回归；随机 seek 的真实耗时仍需测量 |
| 系统音频/双讲 | 端到端声音延迟、丢包下断音、双讲削弱、真实 AEC/NS 效果、设备切换中断 | DSP 合成与历史回环不能替代真人声音验收 |
| 恢复和部署 | UAC/锁屏、RDP 会话、混合 GPU/DPI、显示拔插、无视频处理器、驱动恢复 | 需要可控真机环境；不能靠同名函数证明 |

时延目标应另外给出交互型门槛，例如正常局域网 P95、拥塞后的收敛时间和恢复阶段帧龄；应先形成可重复基线，再决定具体数值，避免用“全部帧最终送达”替代实时性指标。
