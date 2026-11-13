# 发射端当前代码与 RustDesk 功能效果对比

审核日期：2026-11-12。发射端指 Windows 被控主机，包含连接准入、画面/音频发送、输入执行、文件/剪贴板和本机服务。接收端只追踪影响发射端判断的协议调用；服务器端不在本轮审核范围。

## 1. 结论及本轮证据

我方已具备完整度较高的 Windows 远控主干。下一步价值最高的是弱网下的画面新鲜度、多人观看的体验隔离、文字画质与 CPU 开销、免安装提权及桌面切换连续性、语音通话 DSP 接入。仅按功能名称继续补齐表格，收益低于把这些高频场景做稳定。

当前没有证据支持“整体画质、延迟或稳定性已超过 RustDesk”，也没有同条件数据证明整体落后。下文分别标注：当前源码可确认的行为、本轮重新运行的结果、旧报告记载的历史数据、尚需实机验证的效果。

| 对象 | 固定基线 |
| --- | --- |
| 我方 | `D:/ds-chcast`，审核时 HEAD `deb64b89f558bd7b7d84abfb870a15a827da218f`（协议 v72）；集成基线 `main` `bea63f9`（协议 v73） |
| 工作区 | 审核开始时只有未跟踪的 `docs/rustdesk-comparison-2026-11-07.md`；未改变生产代码 |
| RustDesk | 官方 `rustdesk/rustdesk` 仓库，2026-11-12 查询 master/HEAD 后取得 `1ac53ccb1b28e62b726db3cbd6042110349c9c2a`；提交日期 2026-11-12 |
| RustDesk 版本含义 | Cargo.toml 标注 1.5.3；这是固定开发提交，不等同于已发布安装包或默认构建开启所有功能 |
| RustDesk 本地副本 | [build/review-rustdesk-ref](../build/review-rustdesk-ref/)，浅克隆并稀疏检出；未编译、未运行 RustDesk |
| 当前测试 | 独立编译当前 `video_qos.cc` 与已有 `video_qos_test.cc`，1 项 CTest、110 条检查全部通过，0.18 秒 |
| 完整构建 | 新目录配置在 `CMakeLists.txt:112` 查找 zstd 包时停止；本环境还没有完成当前版本的完整构建/回归 |
| 旧二进制 | 当前 `build/Release` 多数程序时间为 2026-11-06；旧 CTest 仅注册 54 项，与当前 CMakeLists.txt 不一致，不作为本轮当前源码的运行证明 |

独立测试材料：[临时 CMake 工程](../build/review-sender-qos/CMakeLists.txt)、[完整测试日志](../build/review-sender-qos/results.log)。这些材料及 RustDesk 副本位于忽略的 build 目录；正式报告保存于 docs。

## 2. 先校正旧报告中的结论

1. **HDR 转 SDR 已是双方共有机制。** 当前 RustDesk [dxgi/mod.rs:194](../build/review-rustdesk-ref/libs/scrap/src/dxgi/mod.rs#L194) 和 [hdr.rs](../build/review-rustdesk-ref/libs/scrap/src/dxgi/hdr.rs) 有浮点桌面采集、色调映射及失败回退。我方 [hdr_pass.cc](../src/capture/hdr_pass.cc) 也有实现，不能继续按旧版 RustDesk 宣称我方独有。这不表示任一方具备端到端 HDR 输出。
2. **静态画面渐进刷新也已共有。** RustDesk [static_refresh.rs:31](../build/review-rustdesk-ref/src/server/video_service/static_refresh.rs#L31) 按编码格式、后端、画质分预算；我方 [desktop_stream.cc:1546](../src/host/desktop_stream.cc#L1546) 的预算表与之接近。不能再用“RustDesk 只重复 10 次”代表当前完整刷新策略。
3. **我方独立光标采样、原始截图、单屏 CPU 退化链路、认证后名额分配已存在。** 分别见 [desktop_stream.cc:241](../src/host/desktop_stream.cc#L241)、[host_server.cc:5660](../src/host/host_server.cc#L5660)、[shared_capturer.cc:262](../src/capture/shared_capturer.cc#L262)、[host_server.cc:1184](../src/host/host_server.cc#L1184)。本轮核对实现，不沿用旧报告的开放缺陷认定，也不把旧测试记载算作本轮复验。
4. **第三方 OLE 虚拟文件读取已接入生产工作线程。** [clipboard_worker.cc:89](../src/clipboard/clipboard_worker.cc#L89) 在普通路径文件不存在时调用 [clipboard_oledata.cc:391](../src/clipboard/clipboard_oledata.cc#L391)，后者通过 OleGetClipboard、文件描述符及延迟内容适配读取。仍需 Office/邮件客户端实测，但不能再说“只有发布侧”。
5. **撤销“RustDesk Windows 已有自由手绘白板”的差距认定。** [whiteboard/mod.rs:27](../build/review-rustdesk-ref/src/whiteboard/mod.rs#L27) 只有 Cursor/Clear/Exit；[windows.rs:86](../build/review-rustdesk-ref/src/whiteboard/windows.rs#L86) 每次清空透明画布并绘制当前光标、姓名和点击波纹，[事件处理:213](../build/review-rustdesk-ref/src/whiteboard/windows.rs#L213) 更新光标和波纹，未形成持久笔画路径。我方已有相似提示，见 [cursor_hint_paint.cc](../src/win/cursor_hint_paint.cc)。自由标注可以独立规划，不能以模块名作为参考产品已实现的证据。
6. **保持无限 P 帧、按需 IDR 的既有产品约束。** 我方 `gop_size=0`；RustDesk 录制时使用 240 帧关键帧间隔，见 [video_service.rs:1115](../build/review-rustdesk-ref/src/server/video_service.rs#L1115)。这是设计差异，不建议为对齐参考产品改成固定 GOP 或定时请求 IDR。

## 3. 按功能比较实现与实际效果

| 功能 | 我方当前实现 | RustDesk 当前实现 | 效果判断与提升方向 |
| --- | --- | --- | --- |
| 桌面捕获与降级 | DXGI、GDI、无 D3D 的 GdiCpu；共享帧供订阅者使用 | DXGI/GDI 及平台捕获实现，Windows 有便携服务采集代理 | 基本路线均完整。我方单屏 CPU 退化已接通，合并多屏的外置设备路径仍不支持无设备退化。重点测驱动重置、RDP、锁屏与恢复耗时 |
| HDR、旋转、色彩 | HDR 转 SDR、旋转处理、NV12 颜色空间设置 | HDR 转 SDR、旋转及失败回退 | 双方均有机制。比较应测灰阶、白场、肤色、彩色细字，不能只看着色器存在或 NV12 后的亮度指标 |
| 硬件编码 | oneVPL 与 FFmpeg QSV/NVENC/AMF；H.264/HEVC/AV1；硬件路径避免整帧 CPU 暂存 | VRAM/HWRAM 编码路径；是否具备取决于 feature、依赖和硬件 | 我方具有多厂商路径，不是“只有 Intel 硬编”。GPU 内部复制仍可能存在。重点是实际可用探测、失败回退与各品牌延迟/功耗实测 |
| 软件编码与文字清晰度 | OpenH264；VP9 4:2:0/4:4:4；AV1 只做硬件 | libvpx VP8/VP9、libaom AV1；VP9/AV1 支持软件 4:4:4 协商 | RustDesk 软件格式选择更多。我方已能走 VP9 全色度；先优化现有文字质量与 CPU 开销，再决定是否增加软件 AV1 |
| 画质预设与码率 | 分辨率基础表、质量比例、硬件系数；VP9 也使用非 H.264 硬件系数 | VP9 使用基础码率×质量比例，并随质量调整量化参数 | 同名“均衡”不等价。应按实际码率比较，或明确选择文字/流畅/低功耗场景，避免更高预算被误判为编码效率更好 |
| 静态画面收敛 | 按后端设置有限重复预算，重复帧不计为动态画面 | 当前已有类似后端/画质预算及静态刷新状态机 | 已接近。应测停止滚动后文字多久清晰、需要多少额外流量和 GPU 时间，不能把更多重复帧本身当优势 |
| QoS 与弱网 | 延迟探针、压缩帧取出 ACK、待收帧龄、动态码率和 FPS；可靠视频流 | 延迟控制、视频取帧反馈、共享 QoS；逐帧等待取走 | 双方都有自适应。我方尚缺双机呈现延迟实测；优先控制过期画面和拥塞恢复，而不仅确认压缩帧最终完整送达 |
| 慢端背压 | 每订阅 60 帧/8 MiB 队列；超限丢引用并等 IDR；发送端另有 500 ms 待收窗口限制新入队 | 应用连接通道无界，视频服务逐帧最多等待约 3 秒取帧通知 | 我方有明确队列边界和逐订阅恢复，但计数/字节边界不是时延保证，传输中旧数据也不会因此自动撤回 |
| 多观看者与接入 | 同源共享编码；兼容连接尽量复用编码器；格式切换先准备并证明 IDR | 每屏共享服务，新订阅者触发 SWITCH 重建 | 我方可减少兼容接入重建，但仍请求 IDR，且新观看者首秒将全局 FPS 限到 15。不能承诺接入完全无影响 |
| 多屏与合并 | 独立显示源、多订阅、拓扑变化处理；同 GPU 可合并画布 | 每屏视频服务与显示服务；共享编码选择/QoS | 我方合并视图有价值，但不是无损替代独立屏幕。4096 长边/面积上限会缩小双 4K 文字；跨 GPU 合并不支持 |
| 光标与协作提示 | 独立约 33 ms 采样，形状缓存/位置发送；姓名、颜色、点击波纹 | 独立约 33 ms 光标/位置服务；Windows 协作光标覆盖层 | 机制接近。剩余价值在低 FPS、慢编码、大截图、混合 DPI、桌面切换时的真实到达与呈现延迟 |
| 键鼠、触控、笔 | HID/扫描码、Unicode、相对鼠标、触控和笔；输入桌面跟随、按键状态处理 | input_service、平台输入实现、便携服务输入代理 | 基础能力已具备。优先验证中文 IME、键盘布局、组合键、相对模式和 DPI 坐标；不能把有协议定义当成所有设备已验收 |
| 系统声音与语音通话 | WASAPI、共享 loopback、审核时为 20 ms Opus，集成后为 10 ms Opus/LowDelay、DATAGRAM/PLC、DSP 底层、通话换麦克风 | 音频服务、通常 10 ms 分包、Opus LowDelay、独立编码工作线程与有界 PCM 交接 | 审核时主机通话默认未接入 AEC/NS，本批已接通；双方集包/编码/缓存不同，不能按“有 DSP”直接判断通话效果，仍需真人双讲验收 |
| 原始截图 | 源线程读回 BGRA，异步 PNG，独立 CSHT 流和 512 KiB 在途预算；流 0 语义明确 | 从原始帧生成截图；当前纹理路径可能临时退出 VRAM 并重建 | 我方截图结构可避免为截图重建编码器；源线程 GPU Map 仍可能停顿画面，共享连接带宽仍存在竞争 |
| 录制 | 压缩包直接写 MP4，32 MiB/120 帧队列、10 分钟分段、磁盘检查、按需 IDR | 压缩帧录制、录制时 GOP 调整、录后上传模块 | 我方已具备队列/分段/磁盘控制。关注随机定位和崩溃后可读性；集中上传是明确扩展，不能据此改写常态 GOP 策略 |
| 剪贴板与文件 | 富文本/图像、方向权限、虚拟文件发布/读取、独立传输、压缩、续传、冲突处理 | clipboard/cliprdr、文件任务及权限机制 | 主干已存在。剩余重点是 Outlook/Office 延迟数据源、取消/断线、超大目录、冲突恢复和哈希吞吐，而不是重复补一个“文件传输”入口 |
| 服务、登录及提权 | Windows 服务启动会话 worker；本地 runas 与状态交接；SAS、OS 登录、会话选择 | Windows 服务及便携特权代理；支持 Direct 和管理员 Logon 提权请求 | RustDesk 便携提权路径更丰富。我方免安装普通用户会话的安全桌面连续性及远程管理员凭据提权仍是重要差距，服务模式不能代替便携模式验收 |
| 隐私、虚拟显示、无头 | 隐私覆盖层、显示切换/恢复、守护；Usbmmidd provider；vdd 明确未实现 | 隐私模式、IDD 管理、无头接口；有多种后端代码，当前默认选择 Amyuni | 我方不是“没有隐私/虚拟屏”。优先验证实际签名驱动安装、无物理屏启动、异常退出恢复，再决定扩展 provider |
| 准入与权限 | 密码、人工同意、TOTP、信任设备、票据/权限撤销；认证后才占正式名额 | connection/login 检查、权限、认证及平台访问机制 | 我方主干已具备；预认证连接仍受总传输上限约束。应测超时清理、撤销响应及恢复，不把“名额隔离”扩大为抗所有资源耗尽 |
| 终端、端口转发及运维 | ConPTY、TCP 转发、远程配置、防休眠/壁纸恢复 | 对应服务/平台路径 | 已有具体实现。应验收进程退出、权限撤销、掉线回收和策略一致性；服务器集中管理另行审核 |
| 扩展与平台 | 主机仅 Windows；未找到远程打印、控制方向互换和录后集中上传闭环 | 对应打印服务、SwitchSides 协议及上传模块；有更多主机平台代码 | 属于明确功能面差异，按受众需求排期。驱动、服务配置、构建选项决定参考实现是否可部署使用 |

功能表的源码索引见第 6 节，优先级见下节。表中的“有实现”不表示本轮运行过全部功能。

## 4. 优先改进项

以下 P1/P2/P3 是产品实施顺序，不是安全漏洞严重级别。

### P1-01：把“低延迟”落到画面新鲜度与真实交互指标

**证据。** [desktop_stream.cc:35](../src/host/desktop_stream.cc#L35) 限制每订阅 60 帧/8 MiB，[publish:675](../src/host/desktop_stream.cc#L675) 在数量/字节超限后清队列并请求 IDR，当前没有按队首画面年龄直接清理的判断。60 帧在 30 FPS 下相当于约 2 秒的生产量，这是容量换算，不是已测固定延迟。[quic.cc:5377](../src/net/quic.cc#L5377) 的 500 ms 窗口约束继续送入的帧，不能保证已进入可靠流的旧帧在 500 ms 内消失。

[quic.cc:8512](../src/net/quic.cc#L8512) 在 `receive_frame()` 交出压缩帧时生成 ACK，发生在解码/显示之前。[host_server.cc:5162](../src/host/host_server.cc#L5162) 将探针、取帧和待收年龄送入 QoS，因此目前主要观测传输/消费，不是最终呈现。

**历史结果。** [sender-fixes-2026-11-12.md](sender-fixes-2026-11-12.md) 记载短时单机 netem 80 ms 单向延迟、0–20 ms 抖动、3% 丢包、1 Mbps 下峰值帧龄 2446 ms；当前 [tools/quic.cc:10467](../tools/quic.cc#L10467) 门槛为 3000 ms。本轮没有复跑该测试，该数字不是当前重新测量，也不是 RustDesk 对照测量。

**改进。** 先记录采集、编码、排队、压缩帧取出、解码、呈现各阶段时长，再按帧龄预算决定是否丢弃未发送积压并按需请求 IDR。对已经写入可靠流的数据，需要评估受控换流/终止旧代与解码恢复，不能简单删除中间 P 帧。动态内容应恢复到“最新可解码画面”，而不是只追求旧帧全部最终送达。

**验收。** 同内容、同硬件下测局域网及多档拥塞/丢包的 P50/P95 呈现延迟、最长停格、损伤后收敛时间、网络恢复时间；输入到可见响应单独测。跨机绝对时钟要校准，或使用本地闭环/外部拍摄法。正常链路和故障恢复门槛分开定义。

### P1-02：让新增/慢观看者的影响可控

**证据。** [video_qos.cc:170](../src/host/video_qos.cc#L170) 对观看者请求帧率取最小值，[adjust_fps:261](../src/host/video_qos.cc#L261) 对各端拥塞帧率取最小值，并对任何首秒新端施加 15 FPS 上限；[adjust_ratio:382](../src/host/video_qos.cc#L382) 选择保守质量调整。我方编码和 QoS 是共享设计。

本轮当前源码测试直接确认：新端加入把已稳定的流限制到 15 FPS；两个 60 FPS 用户中慢端确认拥塞后共享流降到 48 FPS；快端的好反馈不能覆盖慢端约束，慢端退出才释放限制。证据见 [测试:148](../tests/video_qos_test.cc#L148) 与 [本轮日志](../build/review-sender-qos/results.log)。这是策略测试，不是真实双机显示帧率。

RustDesk [video_qos.rs:667](../build/review-rustdesk-ref/src/server/video_qos.rs#L667) 同样取保守上限；[video_service.rs:1292](../build/review-rustdesk-ref/src/server/video_service.rs#L1292) 对新订阅触发 SWITCH。因此这既是资源取舍，也是我方可以改善的体验方向，不应说参考产品已经完全隔离。

**改进。** 先隔离新端启动保护，不让成熟端反复经历首秒降帧；再比较“主要控制者优先”和“按网络/解码能力分成有限编码组”的成本。后者需要额外编码会话和显存，应允许回退共享模式。仅给慢端丢包/限速无法自动解决全局 QoS 投票。

**验收。** 两端一快一慢、连续进出、第三端只看其他屏幕时，记录快端 FPS/码率、P95 帧龄、IDR 突发、编码器重建次数及 GPU 会话数。

### P1-03：补齐免安装提权与 Windows 桌面切换连续性

**证据。** 我方 [elevation_handoff.cc:132](../host/elevation_handoff.cc#L132) 通过本地 runas 请求 UAC，并将状态交给提权子进程；[service.cc:261](../host/service.cc#L261) 在服务路径创建会话 worker。RustDesk [connection.rs:3972](../build/review-rustdesk-ref/src/server/connection.rs#L3972)、[portable_service.rs:997](../build/review-rustdesk-ref/src/server/portable_service.rs#L997) 有 Direct 与 Logon 管理员凭据启动便携服务的路径。我方现有 ElevationRequest 没有相应 Logon 凭据工作流。

[input.cc:440](../src/win/input.cc#L440) 约每 250 ms 跟随输入桌面；普通托盘进程打不开 Winlogon 时，输入仍送旧桌面并被丢弃，源码注释明确了该限制。这不是所有部署模式都无法控制 UAC。

**改进。** 将普通托盘、已提权托盘、安装服务三个模式的能力和状态明确展示；按产品目标评估受限的便携特权代理/管理员凭据提权，使失败、拒绝、超时和重连有明确结果。保留原用户上下文、权限撤销和失败清理。

**验收。** 标准用户和管理员、本地同意/拒绝 UAC、锁屏登录、RDP/控制台切换、提权期间断线与子进程失败。全程量化黑屏时间、输入不可用时间、重连成功率及恢复后的权限一致性。本轮没有操作真实安全桌面。

### P2-01：按远控内容校准 VP9 和 4:4:4 的效果

**证据。** [vpx_encoder.cc:35](../src/codec/vpx_encoder.cc#L35) 默认使用 `hardware_bitrate(..., false)`；[desktop_stream.cc:448](../src/host/desktop_stream.cc#L448) 的共享流也这样计算。结合 [vpl_encoder.cc:1264](../src/codec/vpl_encoder.cc#L1264) 的基础表，1080p、Balanced=0.67、未手工限码率且未扣音频预算时，我方 VP9 目标约 2083 kbps；RustDesk [vpxcodec.rs:325](../build/review-rustdesk-ref/libs/scrap/src/common/vpxcodec.rs#L325) 约 1388 kbps，相差约 50%。这是目标预算推导，不是输出码率测量。

我方量化区间固定 0–56、编码器内不丢帧、最多 8 线程；参考 [vpxcodec.rs:74](../build/review-rustdesk-ref/libs/scrap/src/common/vpxcodec.rs#L74) 随质量改量化区间且允许编码器丢帧。不能仅因都使用 cpu-used=7 宣称效果相同，也不能直接认定我方设置更差。

**改进。** 用固定内容对比同实际码率的文字可读性、滚动拖影、编码 P95、CPU/GPU 占用和功耗，分别调优 4:2:0/4:4:4。在现有路径不能满足目标时再试软件 AV1；AV1 软件编码增加 CPU 消耗的取舍必须实测。

**验收。** 1080p/4K，500 kbps/1/3/8 Mbps，编辑器彩色细字、网页滚动、照片和动态视频。以原始 RGB 为画质参考并包含转换/缩放损失。旧 [编码质量报告](encoder-quality-comparison-2026-11-11.md) 比较的是我方两条 QSV 路径，不是与 RustDesk A/B；其中 NV12 后 PSNR 不能覆盖先前色度丢失。

### P2-02：把语音 DSP 从底层能力接到默认通话路径

**证据。** [host_server.cc:8892](../src/host/host_server.cc#L8892) 只为主机通话麦克风设置设备 ID；[audio.h:325](../include/chast/audio.h#L325) 的 reference 为空、echo_cancellation/noise_suppression 默认 false。[wasapi_mic.cc:65](../src/audio/wasapi_mic.cc#L65) 仅在对应条件成立时创建 DSP，所以默认生产通话并不能因“有 SpeexDSP”就算已启用回声消除/降噪。

审核时 [protocol.h:1089](../include/chast/protocol.h#L1089) 为 20 ms Opus；集成到 v73 后改为 [protocol.h:1113](../include/chast/protocol.h#L1113) 的 10 ms Opus。对应当前 [shared_loopback.cc:50](../src/host/shared_loopback.cc#L50) 每订阅最多积压 25 包，即约 250 ms 音频内容，采集层另有队列。正常情况下未必积满，但容量限制不是低时延预算。RustDesk [audio_service.rs:213](../build/review-rustdesk-ref/src/server/audio_service.rs#L213) 以每秒 100 包处理，[audio_capture_queue.rs:19](../build/review-rustdesk-ref/src/server/audio_service/audio_capture_queue.rs#L19) 有 10 包 PCM 交接预算并使用 LowDelay。

**改进。** 明确主机通话与系统声音消回传的参考信号关系，将播放 PCM、AEC/NS 配置、换设备后的重置接通；按音频年龄淘汰过期包。集成基线已采用 10 ms Opus/LowDelay；仍需结合包开销、丢包和 CPU 数据完成实机验收。

**验收。** 扬声器外放双讲、耳机、稳定底噪、USB 拔插、默认设备切换和拥塞恢复；测声音端到端延迟、断音率、双讲保真和回声抑制。合成信号 DSP 指标不能替代真人双讲。

### P2-03：减少截图、多屏与退化场景对主流的扰动

**证据。** [desktop_stream.cc:170](../src/host/desktop_stream.cc#L170) 截图 CopyResource/Map 在源线程执行；[host_server.cc:5660](../src/host/host_server.cc#L5660) 异步 PNG 与 [quic.cc:5342](../src/net/quic.cc#L5342) 独立 512 KiB 预算已解决旧控制流大包排队问题，但尚未消除 GPU 读回停顿和连接带宽竞争。

[merged_capturer.cc:35](../src/capture/merged_capturer.cc#L35) 将合并图限制在长边 4096、面积 4096×2304；[适配器检查:144](../src/capture/merged_capturer.cc#L144) 要求同 GPU。双 4K 横向 7680 像素合并到此上限必然缩小，清晰阅读应优先独立流。[shared_capturer.cc:262](../src/capture/shared_capturer.cc#L262) 对外置设备拒绝 CPU fallback。

**改进。** 复用/异步化截图读回并保留请求时的目标和权限校验；测大截图期间视频与控制时延。合并视图用于总览，选屏阅读优先独立分辨率；GPU 不可用时考虑明确退回独立 CPU 单屏，而非承诺全 CPU 合并。

### P3：按需求增加产品能力

当前明确可以规划的是远程打印、控制方向互换、录制后集中上传、软件 AV1/AV1 4:4:4，以及其他主机平台。RustDesk 对照入口分别为 [printer_service.rs](../build/review-rustdesk-ref/src/server/printer_service.rs)、[connection.rs:3997](../build/review-rustdesk-ref/src/server/connection.rs#L3997)、[record_upload.rs](../build/review-rustdesk-ref/src/hbbs_http/record_upload.rs)、[codec.rs:402](../build/review-rustdesk-ref/libs/scrap/src/common/codec.rs#L402)。这些功能跨协议、接收端、服务端或驱动，不能只给发射端添加按钮即视为完成。

VP8 主要是格式/兼容选择；我方自有协议并不因此兼容 RustDesk 客户端。不建议仅为了凑齐编码格式增加维护负担。虚拟显示也应先完成现有驱动的部署与恢复验收，再扩展 provider；RustDesk 有多后端代码不表示所有安装包会动态选择所有后端。

## 5. 建议实施顺序与验收交付

| 顺序 | 工作 | 可交付结果 |
| --- | --- | --- |
| 1 | 补全可重复构建所需依赖，固定主机/查看端版本和实际运行文件；建立呈现时延及画质基线 | 可重建的测试产物清单；同条件 A/B 数据；各阶段队列年龄 |
| 2 | 处理帧龄、拥塞清理及恢复；控制新增/慢观看者的全局影响 | 弱网 P95/最长停格下降；快端在慢端加入/退出时的可量化表现；保持按需 IDR |
| 3 | 完成便携提权/桌面切换与音频 DSP 接入 | UAC/锁屏恢复矩阵；外放双讲数据；明确模式能力与失败提示 |
| 4 | VP9/4:4:4、静态收敛、截图和多屏专项优化 | 同实际码率的文字清晰度/编码成本曲线；大截图期间控制/画面延迟；双 4K 总览/阅读差异 |
| 5 | 根据目标客户推进打印、角色互换、录制归档、更多平台 | 每项含权限、失败恢复、部署与跨端验收的完整功能 |

无需等待所有硬件到齐才开始改善。策略和队列可先做可控测试；GPU/双机/安全桌面验收再补实机证据。每个效果结论都应保留机器、驱动、编解码格式、实际码率、内容、样本数和测量位置，避免把某次 PASS 当成产品整体体验达标。

## 6. 其余功能的源码索引

| 功能 | 我方 | RustDesk 固定副本 |
| --- | --- | --- |
| 捕获与色彩 | [dxgi_capturer.cc](../src/capture/dxgi_capturer.cc)、[gdi_capturer.cc](../src/capture/gdi_capturer.cc)、[hdr_pass.cc](../src/capture/hdr_pass.cc)、[nv12_pass.cc](../src/capture/nv12_pass.cc) | [dxgi/mod.rs](../build/review-rustdesk-ref/libs/scrap/src/dxgi/mod.rs)、[gdi.rs](../build/review-rustdesk-ref/libs/scrap/src/dxgi/gdi.rs)、[hdr.rs](../build/review-rustdesk-ref/libs/scrap/src/dxgi/hdr.rs) |
| 编码及协商 | [encoder_factory.cc](../src/codec/encoder_factory.cc)、[encoder.h](../include/chast/encoder.h)、[ffmpeg_encoder.cc](../src/codec/ffmpeg_encoder.cc)、[desktop_stream.cc](../src/host/desktop_stream.cc) | [codec.rs](../build/review-rustdesk-ref/libs/scrap/src/common/codec.rs)、[vpxcodec.rs](../build/review-rustdesk-ref/libs/scrap/src/common/vpxcodec.rs)、[aom.rs](../build/review-rustdesk-ref/libs/scrap/src/common/aom.rs)、[vram.rs](../build/review-rustdesk-ref/libs/scrap/src/common/vram.rs) |
| 显示及订阅 | [display_topology.h](../src/host/display_topology.h)、[merged_capturer.cc](../src/capture/merged_capturer.cc)、[desktop_stream.cc](../src/host/desktop_stream.cc) | [display_service.rs](../build/review-rustdesk-ref/src/server/display_service.rs)、[video_service.rs](../build/review-rustdesk-ref/src/server/video_service.rs) |
| 光标及输入 | [cursor.cc](../src/capture/cursor.cc)、[input.cc](../src/win/input.cc)、[cursor_hint_paint.cc](../src/win/cursor_hint_paint.cc) | [input_service.rs](../build/review-rustdesk-ref/src/server/input_service.rs)、[whiteboard/windows.rs](../build/review-rustdesk-ref/src/whiteboard/windows.rs) |
| 声音 | [shared_loopback.cc](../src/host/shared_loopback.cc)、[wasapi_capture.cc](../src/audio/wasapi_capture.cc)、[wasapi_mic.cc](../src/audio/wasapi_mic.cc)、[opus_codec.cc](../src/audio/opus_codec.cc) | [audio_service.rs](../build/review-rustdesk-ref/src/server/audio_service.rs)、[audio_capture_queue.rs](../build/review-rustdesk-ref/src/server/audio_service/audio_capture_queue.rs) |
| 文件和剪贴板 | [file_transfer.h](../include/chast/file_transfer.h)、[host_server.cc](../src/host/host_server.cc)、[clipboard_worker.cc](../src/clipboard/clipboard_worker.cc)、[clipboard_oledata.cc](../src/clipboard/clipboard_oledata.cc) | [connection.rs](../build/review-rustdesk-ref/src/server/connection.rs)、[clipboard_service.rs](../build/review-rustdesk-ref/src/server/clipboard_service.rs) |
| 录制 | [stream_recorder.cc](../src/host/stream_recorder.cc)、[recording_disk.cc](../src/host/recording_disk.cc) | [video_service.rs](../build/review-rustdesk-ref/src/server/video_service.rs)、[record_upload.rs](../build/review-rustdesk-ref/src/hbbs_http/record_upload.rs) |
| 准入和权限 | [host_security.cc](../src/host/host_security.cc)、[access_ticket.cc](../src/host/access_ticket.cc)、[host_server.cc](../src/host/host_server.cc)、[quic.cc](../src/net/quic.cc) | [connection.rs](../build/review-rustdesk-ref/src/server/connection.rs)、[login_failure_check.rs](../build/review-rustdesk-ref/src/server/login_failure_check.rs) |
| 服务/隐私/虚拟屏 | [service.cc](../host/service.cc)、[elevation_handoff.cc](../host/elevation_handoff.cc)、[display_privacy.cc](../src/host/display_privacy.cc)、[virtual_display.cc](../src/host/virtual_display.cc) | [portable_service.rs](../build/review-rustdesk-ref/src/server/portable_service.rs)、[virtual_display_manager.rs](../build/review-rustdesk-ref/src/virtual_display_manager.rs)、[privacy_mode](../build/review-rustdesk-ref/src/privacy_mode/) |
| 终端和转发 | [terminal_relay.cc](../src/host/terminal_relay.cc)、[port_forward_relay.cc](../src/host/port_forward_relay.cc)、[power_request.cc](../src/win/power_request.cc)、[wallpaper.cc](../src/win/wallpaper.cc) | [connection.rs](../build/review-rustdesk-ref/src/server/connection.rs)、[platform/windows.rs](../build/review-rustdesk-ref/src/platform/windows.rs) |

本轮只新增审核文档和忽略目录内的参考/验证材料，没有修改生产逻辑、安装服务/驱动、操作真实输入/锁屏或提交代码。源码功能面已核对；RustDesk 二进制 A/B、完整当前构建以及双机/多 GPU/安全桌面效果仍待专门验收。
