# Windows 发射端功能审计（main / 协议 v72 vs RustDesk 55786aa）

审计日期：2026-11-12。范围是 Windows 被控主机（发射端）：准入、服务与提权、采集/编码、协商/背压/QoS、光标、音频、截图、录制、多屏、终端/端口转发、剪贴板/文件、隐私与虚拟显示，以及仅主机侧存在的扩展能力。接收端只在确认协议闭环时追踪，不据此声称已审核整个接收端。

> 后续进展（2026-11-12，工作区未提交）：按第 8 节建议推进了两个 P3 项。查看端按形状 ID 重取光标已落地为协议 **v73** 并有自动化；旋转截图降级在实现后复核发现被视频层的同一条件阻塞（旋转帧根本不会进入截图缓存），相关代码已回退，结论见 9.2。本文第 4 节的对照表保留审计时（v72）口径，相关行的“剩余差异”已指向第 9 节。

## 1. 结论

当前 `main` 的发射端主链路完整，未发现新的 P0/P1 缺陷。上一轮审核的 A01（光标采样耦合）、A03（截图占用控制流与流 ID 歧义）、A04（无设备 CPU 回退）、A05（认证前连接占正式名额）修复都在当前代码中成立，A02（录制周期关键帧）仍按“无限 P 帧、按需 IDR”的产品约束撤销。

与 2026-11-11 的文档相比，本轮最大的变化在参照侧：本机 RustDesk 基线已从 `7423dce`（2026-08-22）推进到 `55786aa`（2026-11-03）。新基线补上了 Windows HDR 归一化（`libs/scrap/src/dxgi/hdr.rs`）和按编码器/后端/画质分档的静止刷新（`src/server/video_service/static_refresh.rs`），并重写了 QoS 控制器、音频采集管道和端口转发多路复用。因此旧文档中“RustDesk 没有 HDR / 静止刷新”的占优结论已经过期；我方在静止刷新预算表、QoS 阈值与票序上与参照一致，HDR 双方都是 GPU 归一化到 SDR，不存在“我方独有 HDR 通路”。

剩余差异分为三类：

1. **明确的产品功能缺口**：远程打印、切换控制方向、白板自由标注、软件 AV1/VP8/AV1 4:4:4、非 Windows 主机。
2. **主机侧闭环缺口**：主机收到他人连接时录制只写本地、没有 RustDesk 式录后上传（查看端出站录制已有云端归档）；虚拟显示只有 usbmmidd 一个供应方；便携提权没有 RustDesk 式管理员凭据辅助服务。
3. **设计差异，需要实机 A/B 才知道效果**：VP9 码控（我方固定量化区间、不丢帧；RustDesk 按比例调量化并允许丢帧）、录制无限 P 帧 vs 录制时 240 帧 GOP、端口转发“每转发一条 QUIC 流” vs “多路复用 + 信用窗口”。Opus 帧长已在 2026-11-12 晚对齐为 10 ms（见第 10 节），不再列为差异。

本轮只做源码与自动化验证，没有构建或运行 RustDesk 二进制，没有双机弱网、真实 UAC/锁屏、HDR 目视、物理摄像头或驱动部署验收。

## 2. 基线与方法

| 项目 | 本轮口径 |
|---|---|
| 我方 | `D:/ds-chcast`，`main` HEAD `deb64b89f558bd7b7d84abfb870a15a827da218f`（2026-11-12 17:53），协议 **v72**，审核时工作区无未提交文件 |
| RustDesk | `D:/rustdesk`，HEAD `55786aaa5709ac0a450aabcf06f9d1fee3119908`（2026-11-03）；`7423dce` 仍可按内容检出且不是该 HEAD 的祖先，`c7f212d` 不在本仓库中，均不能代表当前参照 |
| 比较方式 | 两侧源码阅读为主；RustDesk 新增/删除文件用 `git diff 7423dce 55786aa` 的树差异确认。因为本机 RustDesk 是浅克隆，`git log a..b` 的提交数没有意义，本文不据此推断“何时引入” |
| 运行验证 | 我方 8 个测试目标增量构建成功，9 项 CTest 全部通过（62.34 s），日志见第 6 节；RustDesk 只做静态对照 |
| 证据等级 | 本文区分“源码可确认”“自动化通过”“真实链路已测”“待人工/双机验收”。没有把同名函数或测试名称当作效果证明 |

审核路径：从协议枚举和主机入口建立功能表，逐项追踪权限闸门、执行路径、失败/恢复路径和关联测试；对“缺失”同时检查我方协议、主机和查看端调用，避免把“已有但语义不同”误判为缺失。

## 3. 上一轮缺陷在当前 main 的状态

| 编号 | 当前状态 | 代码证据 | 证据等级 |
|---|---|---|---|
| A01 光标采样耦合 | 已修复并保留：显示源有独立指针线程，约 33 ms 轮询，先切输入桌面再采样，直接回调各订阅 sink；捕获帧率、同步编码和发送队列不参与该线程节拍 | [desktop_stream.cc:241](../src/host/desktop_stream.cc#L241) `start_cursor`、[desktop_stream.cc:1394](../src/host/desktop_stream.cc#L1394) 启动处；[host_server.cc:230](../src/host/host_server.cc#L230) 33 ms 去重/节流 | 源码 + `desktop_stream`/`cursor` 自动化 |
| A02 录制周期关键帧 | 维持撤销：主机只在等待 IDR、分段、队列恢复等事件请求关键帧；查看端只在录像器打开时请求一次起始 IDR，其余请求都由流重建、序号缺口、解码错误触发，没有 8 秒周期 | [stream_recorder.cc:193](../src/host/stream_recorder.cc#L193) `needs_keyframe`；[viewer_engine.cc:1475](../app/windows/viewer/viewer_engine.cc#L1475) 起始请求、[viewer_engine.cc:3446](../app/windows/viewer/viewer_engine.cc#L3446) 1 秒节流的事件重试 | 源码；`host_control`/`protocol` 通过 |
| A03 截图占用控制流/流 ID 歧义 | 已修复并保留：v72 分片走专用单向 `CSHT` 流，512 KiB 在途窗口，满窗返回 `WaitTimeout` 由主机下轮重试；`kScreenshotCurrentStream = UINT32_MAX` 与合法流 ID 0 分离；PNG 在辅助线程编码 | [protocol.h:2125](../include/chast/protocol.h#L2125)–[2127](../include/chast/protocol.h#L2127)；[quic.cc:5342](../src/net/quic.cc#L5342)；[host_server.cc:5650](../src/host/host_server.cc#L5650) 起 | 源码 + `host_control` 真实桌面截图断言 |
| A04 无设备 CPU 回退 | 已修复并保留：`GdiCpu` 后端不建 D3D 设备和 Nv12Pass，BitBlt 后输出 CPU BGRA，VP9 直接消费（OpenH264 已于 2026-11-12 移除，软件回退只有 VP9）；截图在 CPU 帧上直接读 | [capture.h:114](../include/chast/capture.h#L114)；[gdi_capturer.cc:61](../src/capture/gdi_capturer.cc#L61)；`desktop_stream` CPU 用例 | 源码 + `desktop_stream` 通过；真实禁用显卡未复现 |
| A05 认证前名额隔离 | 已修复并保留：传输连接上限 = 正式观看名额 + 8；正式名额在 Hello、认证和批准全部通过后才申请并原子检查，关闭时归还 | [host_server.cc:1184](../src/host/host_server.cc#L1184)–[1185](../src/host/host_server.cc#L1185)；[quic.cc:6402](../src/net/quic.cc#L6402) | 源码 + `host_multi` 通过（含不发送 Hello 的连接） |

其余旧结论复核：`GdiCpu`、截图专用流、配额拆分都在；查看端没有周期录制 IDR。旧文档中“8 秒自请求关键帧”的描述必须按本节修正。

## 4. 与 RustDesk 55786aa 的功能对照

“对齐”指两侧都有可追踪的完整路径，不表示效果相同；“差异”只描述机制，不据此宣布优劣。

| 功能面 | chast main（v72） | RustDesk 55786aa | 判断 |
|---|---|---|---|
| 准入、密码、人工同意、TOTP、可信设备 | 密码/点击审批/Both、TOTP、签名可信设备、IP/CIDR、会话票据与撤销 | 连接审批、TOTP、`server/connection/trusted_devices.rs` 的 v2 信任证明 | 对齐；我方另有独立票据/租约设计，本轮不评价服务器策略分发 |
| Windows 服务、工作进程、会话选择 | 服务 + worker + WTS 会话选择 + 登录代次 + 服务按交互用户启动隐藏管理器 | Windows 服务、桌面进程、连接管理器 | 对齐（部署验收另计） |
| 会话内提权 | 查看端请求 → 本地 UAC → 父子进程身份/内存配置校验 → 自动重连；无 SYSTEM 便携辅助服务 | `ElevationRequest` Direct/Logon，`portable_service.rs` 可带管理员凭据启动便携服务 | **差异（G06）**：无便携特权代理与远程管理员凭据工作流 |
| 采集 | DXGI（含 HDR/WCG pass）→ GDI → `GdiCpu` 三级；旋转 pass；合并多屏画布 | DXGI（`DuplicateOutput1` + `hdr.rs` 归一化）→ GDI；旋转；无主机侧合并 | 采集与回退对齐；**合并多屏为我方独有**；HDR 双方都是“归一化到 SDR”，都不是端到端 HDR |
| 编码后端与格式 | oneVPL → FFmpeg NVENC/AMF/QSV → 软件 VP9（libvpx；OpenH264 已于 2026-11-12 移除）；VP9 4:2:0/4:4:4；AV1 仅硬件 | VRAM/HWRAM（hwcodec）→ libvpx VP8/VP9 → libaom AV1；VP9/AV1 可 4:4:4 | **差距收窄**：软件回退与 RustDesk 同为 VP9；仍缺软件 AV1、VP8、AV1 4:4:4（G08 剩余项）。硬件优先是有意取舍 |
| 画质/码率 | 预设表 × 画质比例 × 编码器系数；VP9 固定 `rc_min/max_quantizer=0/56`、`rc_dropframe_thresh=0`、`rc_buf_sz=1000` | 同源码率公式；VP9 按 ratio 调量化区间并允许丢帧（`rc_dropframe_thresh=25`） | **设计差异（B02）**：相同预设名不等于相同码率/量化/连续性，需同内容实测 |
| 编码协商与新观看者 | 全局交集 + 多数票；`prepare → 证明 IDR → commit`，不重启现有流；跨源协商期间暂停显示服务 | 全局交集；已有观看者时新订阅触发 `bail!("SWITCH")` 并重建编码器 | 我方在多人陆续加入时连续性更好；但事务协商仍会同步暂停，不能表述为“零停顿” |
| 帧节奏与背压 | 每订阅 60 帧/8 MiB 有界队列；溢出清队列等 IDR；全部满时暂停采集；控制流/视频流分离 | 每连接无界通道 + 每帧最多 3 秒等待所有连接取走；控制消息与视频同一连接消息流 | 我方隔离性和内存上界更好；慢连接仍通过全局 QoS 拉低整体画质/FPS（B01） |
| QoS 控制器 | 全主机共享；150 ms 阈值、0.95/0.9/0.7/0.85/0.5/0.8 阶梯、3 秒冷却、单次 +150 kbps、600 ms 快速恢复回滚、20 样本基线窗口、启动加速/刹车、恢复后 5 样本守卫 | 55786aa 重写为同一组阈值与票序，并新增启动加速刹车、恢复守卫、失败恢复 600 ms、20 样本基线 | 机制基本对齐；本轮仍无跨产品同条件的弱网体感对比 |
| 静止刷新 | 按编码器/后端/画质预算重复编码（VP9 40/20/20/20；H.264/HEVC 按 NVENC/QSV/AMF/Other；AV1 关闭），首次与 VP9 ≥100 ms、其余 ≤30 fps，VP9 容忍无输出、其他 3 次失败停、100 次无输出停；刷新帧不进入动态画面统计 | `static_refresh.rs` 的 `max_repeat_frames()` 与原表一致，含 VP9 例外和相同的失败/无输出上限 | **对齐**；旧文档“RustDesk 仅 WouldBlock 重复 10 次”只适用于 7423dce，已过期 |
| 光标 | 独立 33 ms 采样线程；按内容 ID 的形状缓存；33 ms 去重发送；显示跟随（前台窗口/指针）由主机轮询并切换订阅；“显示我的光标”覆盖层按设备名取色、绘制姓名、420 ms 点击波纹，点击穿透；v73 工作区补查看端按形状 ID 重取（`CursorShapeRequest`，见第 9 节） | 33 ms `cursor`/`pos`/`window_focus` 服务；形状按内容 ID 缓存，支持 `Misc::request_cursor_data` 重新索取；白板光标带颜色/姓名/点击事件 | 产品面接近；重取闭环已在 v73 工作区补齐（自动化通过，真实双机未测）。**剩余差异**：自由标注未实现 |
| 系统音频/通话 | WASAPI 共享 loopback 扇出；Opus 48 kHz **10 ms**、`OPUS_APPLICATION_RESTRICTED_LOWDELAY`（2026-11-12 晚对齐，见第 10 节）；DATAGRAM + Opus PLC；SpeexDSP AEC/AGC/NS（AEC 参考同一路 loopback）；2 s 静音门；主机语音通话、通话中换麦克风 | 新的 `audio_service/audio_capture*` 重采样/声道重排管道；Opus `LowDelay` **10 ms**；约 8 s 零门；可靠流；无 AEC/DATAGRAM | **B03 剩余差异**：帧长与低延迟应用已对齐；我方传输与 DSP 更完整，静音门更短（2 s vs ~8 s），码率仍显式设置（回环 128 kbps、麦克风 32 kbps，RustDesk 用 libopus 默认）；不能用代码量推断整体通话更好，仍需实机 ABX |
| 主机截图 | v72 专用 `CSHT` 流：显示源线程读回 BGRA、辅助线程 PNG、128 KiB 分片、512 KiB 在途、24 MiB 上限、当前流哨兵；失败/超时回退查看端解码帧；仅已订阅流可截 | `ScreenshotRequest`；VRAM 路径会临时关闭 VRAM 并 `SWITCH` 重建以取得原始像素 | 我方不打断视频流；RustDesk 在 VRAM 路径上打断但覆盖场景更宽。旋转帧的截图层降级经 v73 复核不可达——同一条件会让视频流先失败，见 9.2 |
| 主机/查看端录制 | 压缩包直写 MP4；10 分钟分段；32 MiB/120 帧队列；无限 P 帧、按需 IDR；卷剩余 <512 MiB 拒绝开段；查看端录制已有云端上传/配额/API Token | 压缩包直写；录制时关键帧间隔 240 帧；`hbbs_http/record_upload.rs` 可录后上传 | 机制相近。**差异**：录后上传只覆盖查看端出站录制，主机侧入站录制没有集中归档；长 GOP 的随机 seek 代价需实测 |
| 多屏/拓扑/显示模式 | 每屏独立 Source + 每连接订阅；合并画布；`ListDisplayModes`/`SetDisplayMode`；会话结束由 `ModeRestoreGuard` 恢复原模式；显示跟随窗口/指针 | 每屏独立 `VideoService`；`display_service` 分辨恢复与显示变化检测；`follow_current_display` | 对齐；我方多“合并视图”和主机侧跟随切换，RustDesk 的逐屏服务更简单 |
| 终端 | ConPTY、持久终端、回放 | `terminal_service` | 对齐（用户令牌/退出场景仍待实测） |
| 端口转发 | 每个转发一条 QUIC 流；最多 8 个目标/32 条连接；64 KiB 分片；以本地 socket 接收窗口做背压 | `port_forward_mux.rs`：单隧道多通道 + 信用窗口/接收窗口/通道上限 | **设计差异**：两种流控模型都成立；未做同条件吞吐/队头阻塞对比 |
| 剪贴板/文件 | 文本/HTML/RTF/DIB；方向策略；按需文件剪贴板；空目录；第三方 OLE 虚拟源读取；分页；隐藏项；多共享根；CFIL v2 压缩；递归删除；冲突协商 | clipboard service + cliprdr；FILEDESCRIPTOR/FILECONTENTS 延迟读取；文件任务 | 主要能力对齐；真实 Outlook/Office 虚拟附件与跨网吞吐仍待人工 |
| 隐私/虚拟显示 | 遮罩、BlockInput、隐私拓扑守卫、usbmmidd；`vdd` 明确 Unsupported；驱动包签名/路径检查 | MAG、RustDesk IDD/Amyuni 后端、无头与对端请求创建显示 | **差异（G09）**：我方驱动后端更窄；不能凭代码宣称发布包已含可用驱动 |
| 防休眠/壁纸 | 按连接 PowerRequest；默认关闭的会话期间移除壁纸，备份/恢复/崩溃后恢复 | 入站连接 WakeLock；`allow-remove-wallpaper` | 对齐（真实电源策略/壁纸视觉切换待人工） |
| 扩展能力 | 无远程打印、无切换控制方向、白板仅光标提示、主机仅 Windows | `printer_service.rs`、`SwitchSidesRequest`、`src/whiteboard`、多平台主机 | **产品缺口**，见第 5 节 |

RustDesk 55786aa 的其他新增（`login_failure_check.rs` 合并、`display_service` 增量、`trusted_devices` 拆分、Wayland/DRM/uinput）主要服务非 Windows 平台或属于内部重构，不改变上表的 Windows 发射端结论。

## 5. 剩余差距与建议顺序

### 5.1 产品功能缺口（需要单独立项）

1. **远程打印**：RustDesk 有 `printer_service.rs` 和 `PrinterData`/动态打印驱动适配器；我方协议与主机都没有对应路径。依赖驱动部署，只有明确打印需求时再做。
2. **切换控制方向**：RustDesk `SwitchSidesRequest` 拉起反向连接并关闭原会话；我方没有协调协议。涉及双端、授权和重连恢复，不是主机加一个按钮。
3. **白板自由标注**：我方 v71 已有颜色/姓名/点击波纹覆盖层，缺“留下笔迹/清除/退出”的绘制工具。可复用现有 `ShowCursor` 控制通道新增绘制消息，但需要主机覆盖层绘制与查看端工具栏。
4. **软件 AV1 / VP8 / AV1 4:4:4**：我方 4:4:4 只有 VP9；无 AV1 硬件的机器只能协商 H.264/HEVC/VP9。是否引入取决于目标解码端与 CPU 预算，不应仅为表格对齐。
5. **非 Windows 主机**：产品面缺口，不在本 Windows 对照内。

### 5.2 主机侧闭环缺口

6. **主机入站录制的集中归档**：查看端出站录制已有上传（[session_page.dart](../app/lib/pages/session_page.dart)、`server/internal/account/recordings.go`），主机 `record_incoming` 只写本地 MP4。若需要“主机侧审计录像自动上云”，要单独设计上传身份、配额和断点续传。
7. **虚拟显示后端**：只有 usbmmidd；RustDesk 有 IDD/Amyuni 两个后端。先把现有驱动的安装/卸载/崩溃恢复验完，再决定是否增加供应方。
8. **便携提权**：保留当前 UAC 交接；若产品要求未安装服务也能连续处理安全桌面，再设计受限特权辅助进程，不能直接复制凭据传递。
9. **光标形状重取（v73 已闭环）**：RustDesk 允许查看端用 `Misc::request_cursor_data` 按内容 ID 重取形状。v73 新增 `ControlType::CursorShapeRequest`（88）：查看端在本地形状缓存未命中时按（流、形状 ID）请求，主机从最近一次完整 sprite 缓存重发；未知/过期形状忽略，由下一次指针采样带回当前形状。查看端对同一（流、形状）限速 1 次/秒。协议往返与主机重发已有自动化，见第 9 节。

### 5.3 设计差异与待实测项（不是缺陷）

10. **VP9 码控**：我方固定量化区间 0–56、`rc_dropframe_thresh=0`；RustDesk 按比例调量化并允许丢帧。需要同内容、同实际码率、同 CPU 约束比较清晰度/连续性/延迟。
11. **Opus 帧长（已对齐，转实测）**：2026-11-12 晚已改为 10 ms + `RESTRICTED_LOWDELAY`，与 RustDesk 一致；码率仍显式设置（回环 128 kbps、麦克风 32 kbps），RustDesk 用 libopus 默认。剩余是效果问题：应在真实双讲和弱网下测端到端声音延迟与音质（ABX），不能按帧长或码率单方面判定优劣。
12. **录制 GOP**：我方按产品约束保持无限 P 帧、按需 IDR；RustDesk 录制时 240 帧。应在 10 分钟分段 MP4 上测随机 seek 的解码帧数与耗时，再决定是否调整分段策略。
13. **端口转发流控**：每转发一条流 vs 多通道信用窗口；需要并发大流量下测吞吐与队头阻塞。
14. **认证前传输槽**：正式名额已隔离，但未认证连接仍共用固定 `+8` 传输槽，没有按来源分配；未做压力测试，不声称已抵御资源耗尽。
15. **截图读回**：GPU→CPU 读回仍在显示源线程串行执行；大图并发时光标/权限响应延迟未单独测量；旋转帧在旋转 pass 不可用时仍会被拒绝。v73 曾按 `rotation_degrees` 实现 CPU 旋转并加了几何单测，但复核发现旋转帧不会进入截图缓存、视频流会先失败，代码已回退；真正闭环需要先改变视频层（见 9.2）。

## 6. 本轮构建与测试

增量构建以下目标成功（VS 2026 Release，日志 `build/audit-20261112-build.log`，`cursor_hint` 单独构建日志 `build/audit-20261112-build-hint.log`）：

```text
chast-protocol-test chast-desktop-stream-test chast-cursor-test
chast-cursor-hint-test chast-host-control-test chast-host-multi-test
chast-host-voice-test chast-host-settings-test chast-display-subscription-test
```

随后执行：

```text
ctest --test-dir build/root -C Release -R
  '^(protocol|desktop_stream|cursor|cursor_hint|host_control|host_multi|host_voice|host_settings|display_subscriptions)$'
  --output-on-failure
```

结果 **9/9 通过，62.34 秒**（日志 `build/audit-20261112-ctest.log`）：

| 测试 | 耗时 | 覆盖与本轮意义 |
|---|---|---|
| protocol | 0.06 s | 截图哨兵/分片、光标、显示模式等消息往返与边界 |
| desktop_stream | 14.55 s | 独立光标线程、全队列背压、CPU 编码/采集、静止刷新 |
| cursor | 0.03 s | 指针形状解码/合成 |
| cursor_hint | 0.01 s | 协作光标覆盖层光栅化 |
| host_control | 6.68 s | 真实桌面截图、显示模式、控制消息 |
| host_multi | 38.72 s | 传输/认证名额、多人会话 |
| host_voice | 1.65 s | 通话/麦克风换源路径 |
| host_settings | 0.15 s | 设置持久化（含语音麦克风、壁纸） |
| display_subscriptions | 0.47 s | 显示订阅/跟随/拓扑 |

证据边界：这些是单机自动化测试，`host_control`/`host_multi` 中的“真实桌面”也只在本机回环；没有运行 RustDesk 二进制，没有双机 A/B、真实 HDR、UAC/锁屏、物理摄像头、驱动安装或公网弱网数据。旧文档中的实测数字不能被本轮结果替代。

## 7. 对既有文档的修正

本轮按“保留历史记录、追加日期说明”的方式修正以下过期结论：

- [host-rustdesk-review-2026-11-11.md](host-rustdesk-review-2026-11-11.md)：G04 与功能表不再声称查看端“每 8 秒请求 IDR”；补充参照基线已推进到 55786aa。
- [status-and-plan-2026-11-09.md](status-and-plan-2026-11-09.md)：2026-11-11 的 A02 条目追加撤销说明。
- [sender-audit-rustdesk-2026-11-11.md](sender-audit-rustdesk-2026-11-11.md)：补充基线变化；把“录制后上传”限定为“主机入站录制无上传，查看端出站录制已有”。
- [sender-effectiveness-rustdesk-2026-11-11.md](sender-effectiveness-rustdesk-2026-11-11.md)：补充 RustDesk 55786aa 已有 HDR 与静止刷新、查看端已有录制上传；旧矩阵中的“RustDesk 无 HDR/仅重复 10 次”按历史版本理解。2026-11-12 的效果复核见该文第 16 节；同日编码器同帧实测（H.264 画质略高但码率高 34–39%，1440p/1080p 吞吐低于 30 fps；VP9 吞吐约 3.3×，见 [encoder-quality-comparison-2026-11-11.md](encoder-quality-comparison-2026-11-11.md) 追加节）。基于该实测，当日晚已把产品软件回退改为 VP9、移除 OpenH264，并把 Opus 改为 10 ms 低延迟应用（见本文第 10 节）。
- [host-rustdesk-module-comparison-2026-11-08.md](host-rustdesk-module-comparison-2026-11-08.md)：静止刷新一行的“snapshot + 受限重复编码”更新为 55786aa 的 `static_refresh.rs`。

## 8. 建议的后续动作

1. 先做**双机效果验收**（不新增功能）：1080p/4K、H.264/HEVC/VP9、静态/滚动/视频，记录帧龄、码率稳定、CPU/GPU、彩色细字可读性；这是当前证据缺口最大的部分。
2. 并行做**P3 小闭环**：光标形状重取（v73 已完成，第 9 节）、主机入站录制上传（若产品需要）、旋转截图在旋转 pass 失败时的降级（v73 复核为视频层阻塞，未落地，见 9.2）。
3. 产品确认后再启动**大项**：远程打印、切换控制方向、白板自由标注、软件 AV1/VP8。每项都需要双端协议与部署设计，不与 Windows 稳定性修复混做。
4. 保持无限 P 帧、按需 IDR；若要改善录像 seek，优先评估分段长度/索引，而不是恢复周期 GOP。

## 9. 本轮 P3 进展与结论（协议 v73，工作区未提交）

### 9.1 查看端按形状 ID 重取光标

- 协议：`ControlType::CursorShapeRequest`（88），载荷 16 字节：流 ID（u32，`kCursorCurrentStream = UINT32_MAX` 表示当前聚焦流，其余值按字面量）、4 字节保留（必须为 0）、形状 ID（u64，0 表示主机当前形状）。主机只接受恰好 16 字节且保留位为零的载荷；重复请求无害。
- 主机：`CursorPublisher` 保留最近一次携带 sprite 的完整更新（`shape_cache_`）。会话线程消费请求，把流解析为当前视频或 `other_videos` 中的发布器；`resend_shape` 绕过 33 ms 位置节流，仅当缓存形状仍是当前形状时重发，否则丢弃（下一次指针采样会带回当前形状）。`route()`/`reset()` 在流或模式变化时清空缓存，避免把上一块屏幕的光标发到新流。
- 查看端：`ViewerEngine::RequestCursorShape` 仅在 `kStreaming` 接受，按（流、形状）1 秒限速，由会话线程调用 `QuicSession::request_cursor_shape`。两个触发点：单流 `RemoteCursor::Activate` 缓存未命中（[viewer_bridge.cc:3460](../app/windows/viewer/viewer_bridge.cc#L3460)），多屏 `CollectCursors` 收到空 sprite 的更新（[shared_viewer_session.cc:113](../app/windows/viewer/shared_viewer_session.cc#L113)）。会话结束时清空待发槽。
- 证据：`protocol` 的 `test_cursor_shape_request` 覆盖默认哨兵、固定 16 字节载荷、往返（哨兵与显式流/形状）、非零保留位、每种截断和尾随字节；`host_parity` 多屏光标用例新增四项断言：按 ID 重取收到缓存 sprite、未知形状被忽略、另一查看者的选择不受影响、`kCursorCurrentStream` 解析到所选屏幕。
- 边界：只缓存最近一个形状；请求更早的形状会被忽略，依赖下一次指针采样恢复。真实查看端缓存驱逐场景（长会话、数百个形状）未人工复现。

### 9.2 旋转截图降级：复核结论为视频层阻塞（未落地）

- 审计假设：DXGI 采集器在 GPU 旋转 pass 不可用时发布未旋转 surface 并置 `FrameInfo::rotation_degrees`，截图按该角度在 CPU 旋转即可降级。
- 实现尝试：新增 `detail::rotate_bgra`（复用 `surface_to_output` 的 90 度顺时针契约），在 `copy_screenshot` 的 CPU 帧和 GPU 读回后统一应用，并加了四象限几何单测；`capture_transform`、`protocol`、`host_parity`、`desktop_stream`、`display_video` 定向 5/5 通过。
- 复核发现（未通过）：旋转帧在视频循环里先被拒绝——[desktop_stream.cc:924](../src/host/desktop_stream.cc#L924) 在 `rotation_degrees != 0` 时不再 `remember()`，3 帧后置 `fatal`；[host_server.cc:3990](../src/host/host_server.cc#L3990) 也在编码前直接失败。也就是说，截图缓存里永远不会出现带旋转的帧，`copy_screenshot` 的 CPU 分支在生产路径不可达；单流会话在同样条件下直接结束，多流会话进入恢复重试。原审计引用的“截图拒绝旋转帧”是防御性检查，实际先失败的是视频流。
- 处置：CPU 旋转相关代码与测试已回退（不保留不可达路径）。真正闭环需要先改变视频层，两个方向：一是查看端按 `DisplayInfo.rotation_degrees` 旋转未旋转视频帧（保持零拷贝，需要 Flutter/原生查看端与新协议语义验证）；二是采集端在缺少 GPU 旋转能力时做软件旋转后再进 NV12 管线（增加 CPU 拷贝，与零拷贝约束冲突）。这是独立立项，不属于 P3 小改。
- 现状：旋转显示器在 GPU 旋转 pass 失败时仍无法串流/截图；真实旋转显示器未做人工验收。

### 9.3 验证记录

- 构建：`chast-protocol-test`、`chast-capture-transform-test`、`chast-host-parity-test`、`chast-desktop-stream-test`、`chast-display-video-test` 均 Release 构建成功（`build/build-v73.log`）。
- CTest 定向：`protocol`、`capture_transform`、`host_parity`、`desktop_stream`、`display_video` 5/5 通过（139.26 s，`build/ctest-v73.log`）。
- `host_parity` 直接运行：1016 个 `ok`、0 个 `FAIL`、`0 failed scenario(s)`；新增四项断言为 `viewer can ask for a cursor sprite again`、`host resends a cached sprite without waiting for pointer motion`、`a request for an unknown cursor shape is ignored`、`legacy cursor re-request resends the selected screen's sprite`（`build/host-parity-v73.log`）。
- 回退旋转代码后重建 `capture_transform`/`desktop_stream` 并复跑 2/2 通过（14.76 s），保证记录与最终工作区一致。
- 全量：所有目标 Release 构建成功（0 个 `error C`，`build/build-v73-all.log`）；CTest 74 项 100% 通过（972.39 s，`build/ctest-v73-all.log`），`clipboard_listener`/`clipboard_listener_startup` 按既有配置 Skip，与 v67/v68 全量记录一致。
- 证据边界：查看端 `RequestCursorShape` 的两个触发点只验证到编译与主机往返（`display_video` 目标编译 `viewer_engine.cc`）；真实查看端缓存驱逐（长时间会话、数百形状）与双机链路未验收。

## 10. 2026-11-12 晚：软件回退改为 VP9、移除 OpenH264、Opus 对齐 10 ms

### 10.1 代码

- **视频**：删除 `src/codec/openh264_encoder.cc`，移除 CMake 中的 OpenH264 导入与链接；`EncoderBackend::Software` 只接受 VP9，其他格式明确报错，不静默换格式。`Auto` 顺序为 oneVPL → FFmpeg NVENC/AMF/QSV → 软件 VP9（[encoder_factory.cc](../src/codec/encoder_factory.cc)）。
- **协商与恢复**：硬件连续失败后 `DesktopStream` 只协商软件 VP9；`GdiCpu` CPU 采集只允许 VP9；固定 `Vpl`/`Ffmpeg` 的 VP9 请求继续报错不回退（[desktop_stream.cc](../src/host/desktop_stream.cc)）。
- **音频**：`kOpusFrameSamples` 960 → 480，Opus 改为 `OPUS_APPLICATION_RESTRICTED_LOWDELAY`，编解码按 10 ms 一块打包与补帧；删除 `AudioEncoderConfig::voice`，回环/麦克风仍分别显式 128/32 kbps（[protocol.h:1113](../include/chast/protocol.h#L1113)、[opus_codec.cc](../src/audio/opus_codec.cc)、[audio.h](../include/chast/audio.h)）。
- **测试替身**：软件后端默认产出 VP9，音频头帧长取协议常量；相机恢复、CPU 采集、运行时恢复用例改按 VP9 断言（[host_media_stub.h](../tests/host_media_stub.h)、[desktop_stream_test.cc](../tests/desktop_stream_test.cc)、[host_parity_test.cc](../tests/host_parity_test.cc)、[audio_codec_test.cc](../tests/audio_codec_test.cc)）。

### 10.2 验证

- 构建：`chast-protocol-test`、`chast-desktop-stream-test`、`chast-host-parity-test`、`chast-audio-codec-test`、`chast-host-voice-test`、`chast-host`、`chast-viewer-probe`、`chast-quic`、`chast-encode` Release 构建成功，退出码 0（`build/build-align.log`、`build/build-align2.log`、`build/build-align3.log`）。
- 定向 CTest：`protocol`、`desktop_stream`、`host_voice`、`audio_playout`、`audio` 通过；`host_parity` 首次出现与本次改动无关的剪贴板撤销用例超时（单独运行该场景与随后整套复跑均通过，127.88 s），按偶发时序处理并保留记录（`build/ctest-align.log`、`build/ctest-align2.log`、`build/ctest-hostparity3.log`）。
- 全量 CTest：74/74 通过（979.81 s），`clipboard_listener` 与 `clipboard_listener_startup` 按既有配置 Skip（`build/ctest-align-all.log`）。
- 边界：单机自动化；没有在无硬件编码器的机器上验证 VP9 回退，没有双机真实链路、查看端软解 CPU 预算或音频 ABX；未运行 RustDesk 二进制。
