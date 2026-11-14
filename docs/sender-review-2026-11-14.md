# 发射端代码审核：与 RustDesk 的功能缺口和性能对齐（2026-11-14）

审核对象：Windows 被控主机（发射端）的采集、编码、帧节奏、背压、QoS、传输与媒体外设路径。
接收端与服务器端只在确认发射端闭环时追踪，不作结论。

## 0. 基线与证据边界

| 项 | 本轮口径 |
| --- | --- |
| 我方 | `D:/ds-chcast`，HEAD `a609e44`（协议 v73 之后，2026-11-14），工作区仅未跟踪的测试产物 |
| RustDesk 参照 | `D:/rustdesk-ref`，**1.4.9**（2026-08-24，`7423dce`） |
| 参照版本提醒 | 旧审核文档引用的 `55786aa`（1.5.3 线）与 `build/review-rustdesk-ref` **在本机已不存在**。因此 `video_service/static_refresh.rs`、共享 QoS 重写等更新行为本轮无法复核，下文凡涉及参照侧的判断都以 1.4.9 源码为准，并已标注 |
| 方法 | 源码逐路径阅读 + 与 `docs/sender-*.md` 既有结论交叉验证；**本轮未构建、未运行测试、未做双机 A/B** |
| 证据等级 | 只区分“源码可确认”与“待实测”。下文所有机制结论都有行号；性能数值一律是推导或历史记录，不冒充新测量 |

结论分三档：
1. **应当修的机制缺陷（P1）**：4 项，都在拥塞/慢端恢复路径上，会让弱网表现比参照更差，并有放大效应。**S1–S4 已修复**，见第 2 节末尾的“修复记录”。
2. **已对齐**：采集零拷贝管线、编码参数、码率公式、静止刷新预算、QoS 阈值阶梯。
3. **功能缺口**：8 项，与既有文档一致，本轮逐项在代码里确认“确实没有”。

## 1. 性能对齐：机制级对照

| 环节 | 我方 | RustDesk 1.4.9 | 判断 |
| --- | --- | --- | --- |
| 桌面采集 | DXGI Duplication → 旋转 pass → HDR pass → NV12 pass，全 GPU；GDI → `GdiCpu` 三级回退（`src/capture/dxgi_capturer.cc` `capture()`） | `libs/scrap/src/dxgi/mod.rs` + `gdi.rs`，运行期 `set_gdi()` 回退 | 对齐；我方多合并画布 |
| 编码输入 | 采集直接产出 NV12 纹理，oneVPL 走 `ImportFrameSurface`（优先 SHARED，拒绝后退化为设备内拷贝，`vpl_encoder.cc:845`）；FFmpeg 后端每帧 `CopySubresourceRegion`（`ffmpeg_encoder.cc:742`） | VRAM 编码器直接吃采集纹理；HwRam 路径同样有一次拷贝 | 对齐（同一量级的一次 GPU 内搬运，均无 CPU 往返） |
| 编码参数 | CBR、`GopRefDist=1`（无 B 帧）、`GopPicSize=0`/按需 IDR、`TargetUsage=BALANCED`（`vpl_encoder.cc:638-645`）；FFmpeg `max_b_frames=0`、`gop_size=0`、QSV `rc=cbr,delay=0,forced-idr`（`ffmpeg_encoder.cc:508-570`） | `hwcodec`/`vram` 同为 CBR、无 B 帧、长 GOP | 对齐 |
| 码率公式 | `hardware_bitrate()`：预设表 × 画质比例 × 编码器系数（H.264 2.0 / 非 H.264 1.5，>2 Mbps 衰减），`vpl_encoder.cc:1299` | `scrap::codec::base_bitrate()` + `calc_bitrate()`，同一张表与系数 | **逐位对齐** |
| 静止刷新 | 按 codec/后端/画质分档预算，AV1 关闭，VP9 容忍无输出（`desktop_stream.cc:1643` `static_refresh_budget`） | 1.4.9 只有 WouldBlock 时 `repeat_encode_max=10` | 我方已超过本机参照；`static_refresh.rs` 的对照本轮无法复核 |
| 帧节奏 | 显示服务按 `due` 自调度，编码慢于间隔时跳拍不追赶（`pump()`） | `c.frame(spf)` + `sleep(spf - elapsed)` | 对齐（我方不会因编码慢而连发两帧） |
| QoS | 150 ms 阈值、0.95/0.9/0.85/0.7/0.8/0.5 阶梯（部分阶梯需连续确认）、3 s 冷却、单次 +150 kbps、RTT 基线窗口 20 | 1.4.9：同阈值、无条件 0.95/0.9/0.85/0.8、3 s 冷却、+150 kbps、基线窗口 60/最少 10 | 我方阶梯与 55786aa 线一致（本机 1.4.9 更简单），并多出刹车/恢复守卫；消抖窗口更短，收敛更快也更容易抖动 |
| 拥塞下限 | **fps 最低 5**（`video_qos.cc:11,337,382`） | **fps 最低 1**，响应超 2 s 时压到 2（`video_qos.rs` `MIN_FPS` / `adjust_fps`） | **不一致，见 S2** |
| 背压 | 每订阅 60 帧/8 MiB/250 ms 有界队列；传输另有 60 帧/8 MiB/500 ms 交付窗口；溢出即清队列 + 请求 IDR | 每帧最多等 3 s 等所有连接取走，不丢、不重建关键帧 | **路线不同，见 S1（我方在拥塞时放大流量）** |
| 传输 | control / video / audio / 剪贴板 / 文件各走独立 QUIC 流；音频可走 DATAGRAM | 单一消息通道（视频与控制共流），音频走可靠流 | 我方隔离性更好（无跨类队头阻塞）；音频丢包策略更激进 |
| 订阅取帧 | 会话线程 5 ms 轮询 + 每次 `source->call` 同步派发 | 每连接 mpsc 通道，写入即唤醒 | 我方多 0–5 ms 附加延迟与 200 次/秒唤醒，见 S3 |
| 光标 | 独立 33 ms 采样线程，形状缓存 + 位置增量 | 33 ms `cursor`/`pos` 服务，同机制 | 对齐 |
| 截图 | 显示源线程读回 + 辅助线程 PNG + 专用 CSHT 流 | 请求后可能临时关闭 VRAM 并 SWITCH 重建 | 我方不打断视频流 |
| 音频 | Opus 10 ms + RESTRICTED_LOWDELAY + DATAGRAM + PLC + SpeexDSP AEC/NS | Opus 10 ms + LowDelay、可靠流、无 AEC/DATAGRAM | 我方链路更完整，实机效果未验收 |
| 录制 | 压缩包直写 MP4，32 MiB/120 帧队列，10 分钟分段，仅视频轨 | 压缩包直写 WebM/MP4（`record.rs` 只有 `VideoTrack`），录制期 240 帧 GOP | 轨道构成对齐；GOP 差异属既有产品约束 |

## 2. 需要优先修的四项（P1）

### S1 拥塞/慢端的恢复路径会引发“关键帧风暴”，且没有退避

**路径一（本地队列）**：`expire_backlog()` 在队首画面超过 `kMaxSubscriberQueueAgeMs=250 ms` 时清空该订阅者整条链并 `request_shared_keyframe`（`desktop_stream.cc:885-903`）；`publish()` 在帧数/字节超限时同样清队列并请求 IDR（`:728-736`）；等待 IDR 期间每个非 IDR 画面也会触发一次请求（`:735-738`）。

**路径二（传输窗口）**：交付窗口为 500 ms（`quic.cc:55`、`video_capacity_locked():5401`）。`video_send_ready()` 为假或 `send_frame_nowait` 返回 `WaitTimeout` 时，会话直接进入 `discard_backlog + force_keyframe`（`host_server.cc:5463-5470`、`:4097-4100`）。

**放大机制**：所有请求都只经过 `kIdrMergeWindowMs = 120 ms` 的合并窗口（`desktop_stream.cc:42,369`），没有按订阅者计数或退避。因此单个慢订阅者在稳定拥塞下最多每 120 ms 触发一次共享 IDR（受编码节拍限制，fps ≥ 8 时约 8 次/秒，fps 降到下限 5 时约 5 次/秒）；IDR 是码流峰值（同一 CBR 目标下远大于 P 帧），所以“因为网络满了而丢帧”的处理反而把更多字节塞进同一条管道。参照实现在同等情形下只做等待（≤3 s 取帧），不重建关键帧。

**附加风险**：
- 一个 IDR 画面若自身大于 8 MiB（4:4:4 + 高画质 + 4K 的极端组合，`kMaxFramePayload` 允许到 32 MiB），`queued_bytes + bytes > 8 MiB` 在空队列上也成立，订阅者只能“每轮容纳一个 IDR”，退化为永久关键帧流。
- 观看者路径中断（例如 UDP 黑洞、解码线程卡死）期间收不到 `VideoReceipt`（`quic.cc:3411`），窗口恒为过期状态 → 主机在链路恢复前持续产生 IDR，恢复瞬间把 8 MiB 过期关键帧灌进刚恢复的链路。

**建议**（按代价排序）：
1. 给“重同步驱动”的 IDR 请求加**每订阅者指数退避**（120 ms → 500 ms → 2 s → 5 s，收到该订阅者一次新 `VideoReceipt`/成功交付后退回 120 ms）。改动局部，只在 `Subscription` 加状态。
2. 区分两种来源：传输窗口满时**不要丢弃本地队列**，只暂停生产等 ack（这正是参照的做法）；本地队列过期才允许丢+IDR。
3. QoS 侧同时收到“重同步”信号，让比例/帧率立刻下调一档，而不是等 3 s 冷却。
4. 硬性保护：单帧大于队列字节上限时直接报错而不是每轮循环，避免不可达状态。

**验收门槛**：单观看者、netem 1 Mbps + 80 ms RTT 下，统计 30 s 内 IDR 次数应 < 观看者分数的两倍且随时间下降；同一场景 P95 帧龄与恢复后 5 s 内的收敛时间要可重复。

### S2 拥塞下限与参照不同：5 fps vs 1–2 fps

`kMinAutoFps = 5`（`video_qos.cc:11`）在延迟公式（`:337`）和停滞路径（`:382`）里都是硬下限。参照在同样的延迟计算后允许降到 `MIN_FPS = 1`，并在响应延迟超 2 s 时把全局上限压到 `MIN_FPS + 1 = 2`。

影响：链路被“压干”时（例如 1 Mbps 传 1080p/4K），我方仍以 ≥5 fps 请求编码器，而参照只请求 1–2 fps——后者的余量用于让队列真正排空。历史上 `sender-fixes-2026-11-12.md` 记录的峰值帧龄 2446 ms 出现在 1 Mbps/3% 丢包条件下，与本路径的取向一致（**这是历史记录，不是本轮复测**）。

**建议**：把硬下限改成分档（例如 delay < 600 ms 时 5 fps，≥600 ms 或停滞时允许 2/1 fps），或直接复用参照的 `MIN_FPS` 语义。这是一行常量 + 两处 clamp 的改动，但会改变低带宽场景的行为，需要和 S1 一起验收（S1 修好后下限才有意义）。

### S3 取帧是 5 ms 轮询而不是唤醒

`Capture::capture()` 在 `NoNewFrame` 时 `sleep_for(min(timeout, 5 ms))`（`desktop_stream.cc:1091`），会话线程再回到循环。显示服务发布新画面时没有通知路径。结果：
- 每帧附加 0–5 ms（均值约 2.5 ms）延迟，直接计入端到端帧龄；1080p/30 fps 下相对 rustdesk 的通道唤醒是纯损失。
- 每会话约 200 次/秒唤醒，空转时也照跑。

此外每次 `capture()`/`encode()` 都是一次 `Source::call` 同步派发（打包 `packaged_task` + 两次互斥 + 条件变量），每帧每订阅者 2 次往返。

**建议**：`Subscription` 加条件变量，`publish()`/`pop()` 后 `notify`；会话线程改为 `wait_for(deadline)`。收益是延迟与空转同时下降，且不影响现有队列语义。这是本轮性价比最高的一项。

### S4 观看者可无限制触发共享 IDR

`session_->take_keyframe_request()` 只置 `force_keyframe`（`host_server.cc:6217`），没有每连接限速、没有配额；共享编码器会为所有观看者重编一个 IDR。异常/恶意观看者可以每帧请求一次，把 CPU/GPU 与上行带宽的代价转嫁给其他观看者（参照侧同样没有严格限速，属“对齐但不理想”）。

**建议**：每连接 1 次/秒并在窗口期内合并；连续请求而解码仍失败时按 S1 的退避处理。

### 修复记录（2026-11-14，S1–S4 已修改，未提交）

本节列出上面四项的实际改动与验证方式。“源码可确认”以外，已跑通自动化；**netem 矩阵与双机 A/B 仍未做**，性能结论仍待实测。

| 项 | 改动 | 位置 |
| --- | --- | --- |
| S1 | 每订阅者重同步 IDR 加指数退避（120→240→480→960→1920 ms 封顶 2 s），`expire_backlog`、`publish()` 队列溢出、`discard_backlog()` 三个入口统一走 `request_resync()`；1.5 s 内没有新的重同步**请求**即回落基准步长（按请求时刻计时，持续拥塞不会因为自身退避变长而伪装成已恢复）。被抑制的次数进 `queue_stats().resync_backoffs` → `HostStats.video_resync_backoffs`，现场可观测 | `desktop_stream.cc` `request_resync()`/`resync_delay()`、`desktop_stream.h` `QueueStats` |
| S2 | 帧率下限分档：码率还能降时保持 5 fps（此时减帧不减字节）；码率已到底后按实测延迟 5 → 3（≥600 ms）→ 1（≥1200 ms 或停滞探针），与参照 `MIN_FPS=1` 取向一致 | `video_qos.cc` `auto_fps_floor()`、`delay()`、`pending()` |
| S3 | 去掉 5 ms 轮询：`Source` 加 `frame_mutex`/`frame_wake`/`frame_serial`，`publish()` 后 `notify_all`；`Capture::capture()` 改为“先取发布序号再读一次 → 未命中则等待序号变化（仍以 5 ms 为等待切片）→ 仅当期间确有发布才再读一次”；序号先取避免读与等待之间丢唤醒，超时的等待不多做一次跨线程派发，空转成本与原来的 sleep 持平 | `desktop_stream.cc` `notify_published()`、`Capture::capture()` |
| S4 | 观看者关键帧请求每连接限速 1 次/秒，窗口内请求合并为“到期补发”而不是丢弃；单路与 `other_videos` 各自独立计时 | `host_server.cc` `kViewerKeyframeInterval`、`DesktopVideo::request_viewer_keyframe()`/`promote_viewer_keyframe()` |

验证：
- 新增用例 `resync_backoff_bounds_keyframes`、`publish_wakes_waiting_reader`（`tests/desktop_stream_test.cc`）、`viewer_keyframe_requests_are_bounded`（`tests/host_parity_test.cc`，也可用 `--keyframe-rate` 单独跑）、QoS 下限三条断言（`tests/video_qos_test.cc`）。
- 已确认用例有判别力：临时关闭退避后 `resync_backoff_bounds_keyframes` 失败。
- `chast-desktop-stream-test`（连续 3 次）、`chast-video-qos-test`、`chast-host-parity-test --keyframe-rate` 通过。
- 完整 `ctest -C Release` 本机实测 **62/74**：12 个失败全部是 `quic_*` 的同一句断言 “the host's audio path reported no errors or drops”（`dropped` 1–10、`errors=0`），视频、传输、录制断言全过。把 `Capture::capture()` 临时改回旧的 5 ms 轮询做对照后，同一用例的 `dropped` 反而升到 11–20（3 次），且单跑时该断言时过时不过——属当前机器负载下音频环的抖动，不是 S1–S4 引入的回归。详见下方“仍未覆盖”。
- 顺带修复 `tools/quic.cc` 的 `microphone_player` 工厂签名过期（HEAD 上该目标本就编不过，阻塞所有 quic_* 测试）。

### 后续审核修复（2026-11-14）

本轮复核补齐了三个调用链问题：

- 网络背压恢复后，会话不再额外设置 `force_keyframe`。订阅服务也会将恢复期间的普通关键帧请求纳入同一退避窗口，防止强制编码或重复请求绕过限制。
- 被退避的请求保留 `resync_pending`，由显示服务定时检查并重试；收到 IDR 或重建来源后取消待重试状态。静止 AV1 和刷新预算耗尽的桌面不再依赖下一次画面发布才能恢复，尚有待处理请求时也不会误判为链路已恢复。
- QUIC 验证工具的模拟播放器只接受完整的 48 kHz 单／双声道块，向 `render_sink` 传递帧数、声道数和 steady-clock 播放时间。

本轮重新构建了 `chast-desktop-stream-test`、`chast-host-parity-test`、`chast-video-qos-test` 和 `chast-quic`。验证通过：

- `desktop_stream`：包含重复请求不能绕过退避、静止 AV1 到期恢复、未读 IDR 过期后的再次恢复。
- `video_qos`。
- `quic_microphone_reference`：验证单／双声道回调参数、实际音频参考读取、静音及不支持格式的过滤。
- `chast-host-parity-test --backpressure`：真实 QUIC 交付窗口背压与恢复、会话队列过期遥测。

此处记录的是本轮针对性验证，不表示重新执行了前述 74 项全量测试或双机性能验收。

仍未覆盖（不作为已修复项）：
- 单帧 > 8 MiB 时队列字节上限不可达的退化路径（本轮只加了退避，没有把“不可达上限”变成显式错误）。
- 原建议第 3 条“QoS 收到重同步信号立即降一档”尚未接入；当前重同步只影响关键帧节奏，不影响比例/帧率的 3 s 冷却。
- 拥塞下比例每步仅降 20%（`limit_fps_change` 除数 5），从 30 fps 收敛到下限约需 10 s 量级，弱网首屏恢复偏慢。
- `quic_*` 的“音频零丢弃”断言（`tools/quic.cc:10121` 等）对机器负载敏感：本轮整批串行跑时 12 个用例失败在这一句，视频相关断言全部通过；单跑 `quic_recording_hevc` 时 `dropped` 在 0–10 之间抖动。对照实验：把 `Capture::capture()` 临时改回旧 5 ms 轮询（不含本次唤醒路径）后，`dropped` 为 12/20/11（3 次），比新实现更差，说明丢弃来自音频采集环/共享发送队列在当前负载下的抖动，与 S1–S4 无关。相邻两条断言（模式切换/编码器切换）本就放宽到 ≤8 并在注释里承认同一原因；建议把这条也放宽到同一裕度，或在 CI 上标注为负载敏感。

## 3. 功能缺口（本轮逐项在代码中确认“没有”）

| # | 缺口 | 确认方式 | 参照侧 | 备注 |
| --- | --- | --- | --- | --- |
| G1 | 远程打印 | `src/`、`include/` 全量检索 `printer` 无主机会话路径 | `src/server/printer_service.rs` | 需要驱动部署，按客户需求立项 |
| G2 | 切换控制方向 | 协议枚举无对应控制类型（`include/chast/protocol.h` `ControlType`） | `SwitchSidesRequest` | 跨双端 + 授权 + 重连设计 |
| G3 | 白板自由标注 | 只有 `src/win/cursor_hint_paint.cc`（姓名/颜色/点击波纹） | `src/whiteboard`（1.4.9 仅光标与波纹，无持久笔迹） | 参照能力也有限，按产品需要单独立项 |
| G4 | 软件 AV1 / VP8 | `encoder_factory.cc` 软件后端只接受 VP9 | libaom AV1、libvpx VP8/VP9 | 无 AV1 硬件的机器只能 H.264/HEVC/VP9 |
| G5 | AV1 4:4:4 | `valid_video_format()`：4:4:4 仅 VP9（`encoder.h:57-60`） | VP9/AV1 均可 4:4:4 | 与 G4 一起评估 |
| G6 | 便携特权辅助服务 / 远程管理员凭据提权 | 现有链路为 UAC 交接 + 重连 | `portable_service.rs`（Direct/Logon） | 免安装场景的安全桌面连续性 |
| G7 | 虚拟显示后端只有 usbmmidd | `virtual_display.cc` 只有 usbmmidd provider | IDD/Amyuni | 先验现有驱动的安装/恢复 |
| G8 | 主机入站录制无集中上传 | `stream_recorder` 只写本地 MP4 | `hbbs_http/record_upload.rs` | 查看端出站录制已有上传 |

非缺口但需要澄清的两点：
- **多屏并发推送**：当前一个会话同一时刻只发一路画面，其余显示保持热订阅以便秒切（`host_server.cc` `other_videos` + `swap_video`）。参照 1.4.9 同为“一次一路”，因此按本机参照不算缺口；若要对齐 1.5 线的多屏同显，需要协议与接收端同步改动。
- **主机录制无音频轨**：参照 `record.rs` 同样只有 `VideoTrack`，属对齐而非缺口。

## 4. 已确认对齐或领先的项（避免只列差距）

- 码率公式与编码参数逐项对齐（CBR、无 B 帧、长 GOP、码率系数表）。
- 静止刷新预算表（AV1 关闭、VP9 例外、失败/无输出上限）远超本机参照。
- HDR→SDR、旋转、NV12 全 GPU 管线；无 CPU 往返。
- 控制/视频/音频/文件分流的 QUIC 传输，避免单通道队头阻塞。
- 31 项权限位 + 票据/租约、会话选择、终端、端口转发、摄像头、语音通话、隐私模式、壁纸恢复：主干均在位。
- 指标可观测性（`queue_stats`、`latency()`、`transport_stats()`）比参照更细，可直接支撑下节的验收。

## 5. 建议的验证顺序（能量化对齐，而不是继续补表）

1. **先修 S1/S2/S3**，然后单观看者 netem 矩阵（LAN / 80 ms / 80 ms+3% 丢包 / 1 Mbps / 断网 5 s 恢复）记录：P50/P95 帧龄、IDR 次数、过期帧数、恢复收敛时间、输入到可见响应。
2. 双观看者（快+慢）复跑同一矩阵，观察共享 IDR 对其他观看者的干扰，这是 S1 是否修好的判定场景。
3. 双机 A/B（同硬件、同内容、同实际码率）比较 P95 呈现延迟、最长停格、CPU/GPU 占用；呈现时刻需要接收端埋点或外部拍摄法。
4. 功能缺口按 G1/G2/G3/G8 的商业价值排序立项，不并入稳定性批次。

## 6. 关键代码索引

| 主题 | 位置 |
| --- | --- |
| 订阅者队列/过期/IDR 合并 | `src/host/desktop_stream.cc:36,42,369,459,717,885,1091`；`src/host/desktop_stream.h:28` |
| 显示服务帧节奏与静止刷新 | `src/host/desktop_stream.cc` `pump()`、`static_refresh_due()`、`:1643` |
| 会话侧发送与重同步触发 | `src/host/host_server.cc:4097-4100,5463-5470,6217` |
| QoS 控制器 | `src/host/video_qos.cc:11,337,382` |
| 传输交付窗口与 VReceipt | `src/net/quic.cc:55,3411,5401,8008-8085` |
| 编码参数与码率 | `src/codec/vpl_encoder.cc:638-645,845,1299`；`src/codec/ffmpeg_encoder.cc:508-570,742` |
| 采集管线 | `src/capture/dxgi_capturer.cc` `capture()`；`src/capture/nv12_pass.cc` |
