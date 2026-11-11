# 发射端功能与实现效果对照（chast vs RustDesk）

状态：代码级对照，2026-11-11。chast 侧基线 `main` `ca51be2` 加本轮未提交
改动（协议 v70）；RustDesk 侧基线为本机 `D:/rustdesk-ref`，提交 `7423dce`
（2026-08-22）。


更新说明（2026-11-12）：本文保留早期静态对照，修复后的实现和验证以
[发射端修复记录](D:/ds-chcast/docs/sender-fixes-2026-11-12.md) 为准。
编码策略固定为无限 P 帧、按需 IDR，包括主机和查看端录制；不引入周期 GOP。
截图现使用 v72 专用 CSHT 流，PNG 异步编码，原文控制流描述只适用于旧实现。

口径说明：

- 本文只比较**发射端（主机）**：采集 → 预处理 → 编码 → 节奏/背压 → 发送 →
  光标、音频、截图、录制、多屏等主机侧功能。查看端渲染、输入注入、剪贴板、
  文件传输不在范围内。
- RustDesk 侧只做静态代码审阅，**没有编译或运行其二进制**。因此文中
  “效果”分为三类：可由代码结构直接判断的、chast 侧已有实测数据的、
  以及必须做同内容 A/B 才能下结论的。
- chast 侧实测数据引自仓库内 `docs/encoder-quality-comparison-2026-11-11.md`、
  `docs/shared-video-qos-rustdesk-2026-11-08.md`、
  `docs/weak-network-acceptance.md`、`docs/pointer-acceptance-2026-11-11.md`。
- `docs/host-rustdesk-review-2026-11-11.md` 的基线是旧路径 `D:/rustdesk` 的
  `c7f212d`，并引用了本机基线中不存在的 `libs/scrap/src/dxgi/hdr.rs`。本文
  不复用它的实现描述；凡涉及版本差异会在正文标明。

## 1. 总览

| 维度 | chast | RustDesk 7423dce | 判断 |
| --- | --- | --- | --- |
| DXGI 采集语义 | 指针帧与桌面帧分开标记；光标独立采样 | `LastPresentTime == 0` 视为 WouldBlock；光标走独立服务 | 持平，chast 更细 |
| 静态画面的重复编码 | 按编码器/后端的静止刷新预算，刷新帧不算动态画面 | 仅 WouldBlock 时重复最近 YUV，最多 10 次，且仅限非 latency-free 编码器 | chast 更完整（新版本 RustDesk 另有实现） |
| HDR 桌面 | FP16 scRGB → SDR 像素着色器，失败可回退 | 本基线 Windows 路径无 HDR 处理 | chast 占优（未做目视验收） |
| 旋转/合并多屏 | 旋转 pass；主机侧合并画布（单 NV12 pass，4096 长边/面积上限） | 旋转有处理；无主机侧合并采集 | chast 多一项能力 |
| 编码后端 | oneVPL → FFmpeg NVENC/AMF/QSV → OpenH264 | VRAM/HWRAM（外部 hwcodec）→ libvpx VP9，AV1 用 libaom | RustDesk 覆盖面更宽 |
| 硬件编码零拷贝 | VPL/FFmpeg 全程 VRAM，无 CPU 暂存 | VRAM 路径零拷贝；HWRAM 走系统内存 | 持平 |
| 编码格式 | H.264/HEVC/AV1 硬件 4:2:0；VP9 软件 4:2:0/4:4:4 | H.264/HEVC 硬件；VP8/VP9/AV1 软件；VP9/AV1 4:4:4 | RustDesk 格式更多；chast 为硬件优先策略 |
| 码率/画质公式 | 对齐 RustDesk：预设表 × 画质比例 × 编码器系数 | 原始实现 | 持平 |
| QoS 策略 | 对齐 RustDesk 阈值与票序，增加视频确认/待收年龄样本 | 延迟探测 + 每帧取帧等待 | 持平，chast 样本更多 |
| 新观看者接入 | 组内 prepare → 证明 IDR → 原子 commit；不重启现有流 | 检测到新订阅即 `SWITCH`，重建编码器 | chast 占优 |
| 帧节奏/背压 | 每订阅 60 帧/8 MiB 有界窗口，溢出等 IDR；全部满则暂停采集 | 每连接无界通道；每帧最多等 3 s 让所有连接取走 | chast 隔离性更好；RustDesk 更简单 |
| 控制面与视频面 | QUIC 控制流/视频流分离，视频积压不挡控制 | 同一连接消息流，视频发送阻塞时控制消息排队 | chast 占优 |
| 光标 | 采集线程逐帧采样、去重合并、按显示投递；覆盖层按查看端取色/姓名/波纹 | 成熟光标服务 + 白板 | 产品化接近；自由标注仍缺 |
| 音频 | 共享 loopback 扇出；SpeexDSP AEC/AGC/NS；DATAGRAM + Opus PLC；主机语音通话 | 共享 audio service；LowDelay Opus；8 s 静音门；语音通话；无 AEC/DATAGRAM | chast 传输与 DSP 更强；设备切换未对比 |
| 主机侧截图 | 查看端请求原始帧，主机在显示服务线程旁路拷贝 BGRA、编码 PNG 分片发送；失败/超时回退到解码帧 | 采集循环内取原始帧转 RGBA 发送 | 持平（chast 不打断视频流） |
| 录制 | 编码包直写 MP4，多源、10 分钟分段、32 MiB 队列 | 编码包写入 Recorder，录制时关键帧间隔 240；可选录制后上传 | chast 分段/多源更明确，RustDesk 多上传 |
| 多显示器 | 每屏独立 Source，全局编码/QoS，可合并 | 每屏独立 VideoService，全局编码/QoS | 持平 |
| 主机增值功能 | 有主机侧截图、协作光标颜色/姓名/点击波纹；无自由标注、远程打印、录制上传 | 四者都有 | RustDesk 仍多三项 |

### 1.1 功能缺口清单

本节只判断“功能是否存在、有无代码路径”，不涉及效果验收；机制与效果
见第 2–13 节，验证边界见第 15 节。

**chast 缺少的发射端功能（RustDesk 有）**

| 功能 | RustDesk 7423dce | chast | 影响 |
| --- | --- | --- | --- |
| 白板/协作光标 | [src/whiteboard/mod.rs](../../rustdesk-ref/src/whiteboard/mod.rs) 的 `Cursor/Clear/Exit` 事件，光标带颜色、按键、文字 | v71 起覆盖层有颜色、姓名和点击波纹；无自由标注 | 自由标注缺失 |
| 远程打印 | `OPTION_ENABLE_REMOTE_PRINTER`、`PrinterData`、`JobType::Printer`（[connection.rs:1010](../../rustdesk-ref/src/server/connection.rs#L1010)） | 无 | 打印重定向缺失 |
| 切换控制方向 | `SwitchSidesRequest` 拉起反向连接并关闭原会话（[connection.rs:3646](../../rustdesk-ref/src/server/connection.rs#L3646)） | 无对应协议 | 角色互换缺失 |
| 录制后上传 | [record_upload.rs:23](../../rustdesk-ref/src/hbbs_http/record_upload.rs#L23)，录制时把 `RecordState` 交给 hbbs_http | 只写本地 MP4 | 录制集中归档缺失 |
| 软件 AV1 | libaom（[video_service.rs:1026](../../rustdesk-ref/src/server/video_service.rs#L1026)），可按观看端能力启用 4:4:4 | 只有硬件 AV1，Auto 不产生软 AV1 | 无 AV1 硬件且观看端只声明 AV1 时无法接入 |
| VP8 | libvpx（[video_service.rs:1015](../../rustdesk-ref/src/server/video_service.rs#L1015)） | 无 | 只支持 VP8 的旧客户端无法接入 |
| AV1 4:4:4 | 软编支持（[codec.rs:403](../../rustdesk-ref/libs/scrap/src/common/codec.rs#L403)） | 仅 VP9 支持 4:4:4 | 全色度只在 VP9 可用 |
| 非 Windows 主机 | 主机端覆盖 macOS/Linux/Android | 仅 Windows | 产品面缺口，不在本文 Windows 对照内 |

认证前连接占名额、便携提权、虚拟显示后端属于主机
服务/设备面，不在本文媒体链路口径内；第三方剪贴板虚拟文件源已于
2026-11-11 补齐，其余证据见
[host-rustdesk-review-2026-11-11.md](host-rustdesk-review-2026-11-11.md)
的 G05、G06、G09。

**本轮已补齐（2026-11-11）**

- 独立光标采样：显示源新增指针线程（`Source::start_cursor`），约 33 ms
  轮询位置与形状并回调各订阅 sink；捕获帧率、同步编码阻塞和发送队列满
  都不再推迟采样。`cursor_snapshot` 在独立采样下返回该线程的最新快照。
  `desktop_stream` 新增 5/30 fps + 300 ms 编码阻塞用例：5 fps 下等待
  14.45/46.22 ms、30 fps 下 62.37/16.01 ms，均远低于 150 ms 门槛。
- 主机侧原始帧截图：协议 v70 新增 `ScreenshotRequest`(86)/`ScreenshotResponse`(87)。
  主机在 `DesktopStreamPool::screenshot` 里经显示服务线程把该流保留的 BGRA
  帧拷到 CPU，WIC 编码 PNG，按 128 KiB 分片走控制流；查看端
  `request_screenshot` 收齐后写文件，拒绝/不支持/10 s 超时则回退到本地
  解码帧。截图不经过编码器，也不打断视频流。
- 录制策略按产品约束保持无限 P 帧、按需 IDR。RustDesk 录制时设置 240 帧 GOP，
  属于设计差异。此前“主机和查看端每 8 s 请求 IDR”的补记已撤回，相关定时逻辑
  已移除；仅在录制启动、新文件/分段、引用恢复等事件按需请求 IDR。
- 通话中麦克风换源：RustDesk 用 `set_voice_call_input_device` 重启音频
  服务；chast 的托盘选择改为调用 `HostServer::set_voice_microphone`，
  当前通话在新设备上重启采集，通话身份、查看端解码器和包序号都保持不变；
  新设备格式不一致时保留旧设备并在主机日志说明。

**RustDesk 缺少的发射端功能（chast 有）**

| 功能 | chast | RustDesk 7423dce | 影响 |
| --- | --- | --- | --- |
| HDR 桌面 | FP16 scRGB→SDR 色调映射，失败回退 | 只请求 8-bit BGRA/420_OPAQUE | HDR 桌面颜色由系统 8-bit 转换决定 |
| 主机侧合并多屏 | 单画布、单 NV12 pass | 无 | 查看端需自行排列多路流 |
| 静止画面刷新预算 | 按编码器/后端出刷新帧，且不计入动态画面 | 仅 WouldBlock 时重复 10 帧 | 长时间静止后的恢复依赖编码器自身行为 |
| 新观看者不重启现有流 | prepare→证明 IDR→commit | 已有观看者时新订阅触发 `SWITCH` | 多人陆续加入会中断现有流 |
| 每订阅背压隔离 | 有界队列、溢出等 IDR、全满暂停采集 | 无界通道 + 每帧等 3 s | 慢连接拖慢整屏，内存无上界 |
| 控制面/视频面分离 | QUIC 双流 | 单消息流 | 视频阻塞时输入/剪贴板排队 |
| 音频 AEC/AGC/NS | SpeexDSP | 基线无 | 双向语音回声/底噪 |
| 音频不可靠传输 | QUIC DATAGRAM + Opus PLC | 可靠流 | 弱网音频迟到与断音 |
| 录制分段/多源 | 10 分钟分段，display/merged/camera 标注 | 单文件 | 长录制管理不便 |

平台差异、查看端渲染与输入注入不在本表。

## 2. 采集与颜色管线

### 2.1 帧语义

RustDesk 在 [libs/scrap/src/dxgi/mod.rs:337](../../rustdesk-ref/libs/scrap/src/dxgi/mod.rs#L337)
调用 `AcquireNextFrame`，随后把 `LastPresentTime == 0` 当作
`WouldBlock`。桌面没有新像素、只有指针移动时，视频服务不会编码，
只走 [video_service.rs:805](../../rustdesk-ref/src/server/video_service.rs#L805)
的 WouldBlock 分支：如果编码器不是 latency-free，就把上一帧 YUV
重复编码，最多 10 次（`repeat_encode_max = 10`）。光标本身由独立
光标服务发送，不依赖视频帧。

chast 的 [src/capture/dxgi_capturer.cc:617](../src/capture/dxgi_capturer.cc#L617)
也用 `AccumulatedFrames > 0 && LastPresentTime != 0` 判断桌面是否变化，
但把指针帧仍然发布为“有光标、无桌面更新”的一帧；服务线程在
[src/host/desktop_stream.cc:749](../src/host/desktop_stream.cc#L749)
先 `notify_cursor()`，再根据 `nv12_updated/desktop_updated` 决定是否
编码。纯指针采样不会变成视频帧，也不会进入共享 QoS 的动态画面统计。

效果判断：两者都不会把纯鼠标移动编码成整屏帧，但 chast 的分离更彻底：
光标投递不受视频背压影响，静态桌面上的鼠标移动不会消耗静态刷新预算，
也不会污染 ABR 的“有动态画面”证据。RustDesk 的重复编码只发生在
WouldBlock 且编码器支持重复帧时，本身开销有限，但会把静态画面续传
10 帧后停止；在有新版本 `static_refresh.rs` 的实现里这一行为已被
更细的预算取代，本机固定基线没有该模块。

### 2.2 HDR、旋转与合并

chast 的采集链路在 DXGI 输出 FP16 scRGB 时进入
[src/capture/hdr_pass.cc](../src/capture/hdr_pass.cc) 的像素着色器，
按桌面 SDR 白点把 HDR 内容映射到 SDR，失败时
[src/capture/dxgi_capturer.cc:527](../src/capture/dxgi_capturer.cc#L527)
重新以非 HDR 方式建立 duplication，避免整条流不可用。旋转由独立
pass 处理；合并多屏由 [src/capture/merged_capturer.cc](../src/capture/merged_capturer.cc)
把各屏复制进同一画布，只做一次 NV12 转换，长边上限 4096、面积上限
4096×2304，并对每块屏先做零超时轮询，避免空闲屏拖慢活跃屏。

RustDesk 本机基线的 DXGI 只请求 `B8G8R8A8_UNORM` 和
`420_OPAQUE`（[libs/scrap/src/dxgi/mod.rs:132](../../rustdesk-ref/libs/scrap/src/dxgi/mod.rs#L132)），
没有 FP16 scRGB 输入和显式白点映射；`libs/scrap/src/dxgi/` 下只有
`gdi.rs`、`mag.rs`、`mod.rs`，不存在旧审核文档引用的 `hdr.rs`。HDR 桌面
的颜色由系统的 8-bit 转换决定，chast 则显式做色调映射并提供失败回退。
它也没有主机侧合并采集，多屏由查看端排列。

效果判断：HDR 与合并采集是 chast 的结构性增量，但 HDR 只做了代码与
回退路径审查，没有 HDR 显示器目视验收；合并画布在 4K 多屏下的编码
压力也只有上限约束，没有实测。RustDesk 在普通 SDR 单屏路径上更简单，
没有这两条路径带来的额外故障面。

## 3. 编码器与画质

### 3.1 后端与格式矩阵

chast 的选择顺序在 [src/codec/encoder_factory.cc](../src/codec/encoder_factory.cc)：
VP9 走软件 libvpx；其余格式走 oneVPL，失败后 FFmpeg 厂商编码器
（NVENC/AMF/QSV），Auto 最后落到 OpenH264。硬件路径只接受 4:2:0；
VP9 软件额外支持 4:4:4，4:4:4 时直接读原始 BGRA 并用 libyuv 转 I444
（[src/codec/vpx_encoder.cc:1](../src/codec/vpx_encoder.cc#L1)）。AV1
只有硬件实现，Auto 不会生成软件 AV1；这是“不新增软件 AV1/VP8 后端”
的既定策略，不是遗漏（Auto 在硬件全失败时仍有 OpenH264 软件 H.264
兜底，见 [encoder_factory.cc:100](../src/codec/encoder_factory.cc#L100)）。

RustDesk 的顺序在 [video_service.rs:965](../../rustdesk-ref/src/server/video_service.rs#L965)
的 `get_encoder_config`：H.264/H.265 依次尝试 VRAM、HWRAM，失败退到
libvpx VP9 软件；VP8/VP9 用 libvpx；AV1 用 libaom 软件。
`Encoder::use_i444`（[codec.rs:403](../../rustdesk-ref/libs/scrap/src/common/codec.rs#L403)）
按所有观看者能力决定 VP9/AV1 的 4:4:4。

效果判断：

- 格式覆盖 RustDesk 更宽：软件 AV1、VP8、AV1 4:4:4 都是 chast 没有的。
  对没有 AV1 硬件的机器，chast 只能协商到 HEVC/H.264/VP9；如果观看端
  只声明 AV1，请求会失败。考虑到 chast 的硬件优先策略，这是有意取舍，
  但应在协议/产品层明确，而不是让用户看到“没有共同编码格式”。
- 画质调校 chast 有实测：`docs/encoder-quality-comparison-2026-11-11.md`
  在 1080p H.264 下对比 oneVPL VDENC 与 FFmpeg QSV；对齐到约 2.5 Mbps
  后两路 PSNR-Y 均值都在 55–56 dB、SSIM-Y ≥0.999，并记录了 oneVPL CBR
  漏桶修复（CPB/MaxKbps，修复前实测码率可达目标的两倍以上）。RustDesk
  本机基线没有对应的同内容画质数据，只有码率公式可对照。
- 两端的码率公式一致：`base_bitrate` 先按像素数在预设表中选择最近档
  并等比缩放，再乘画质比例和编码器系数（1080p 预设 2073 kbps、4K 5000
  kbps；H.264 在低码率段系数 2.0，HEVC/AV1/VP9 为 1.5，超过 2000 kbps
  后逐步收敛到 1.0）。chast 在
  [src/codec/vpl_encoder.cc:1260](../src/codec/vpl_encoder.cc#L1260)
  复刻了预设表和像素缩放，并统一按 kbps×1000 返回。

### 3.2 零拷贝与输入路径

chast 的 oneVPL 在 2.10+ 上共享导入或 GPU 复制 D3D11 纹理，FFmpeg
后端把采集纹理 `CopySubresourceRegion` 到 D3D11VA 帧池，QSV 的帧只
映射一次；全链路不经过系统内存（[src/codec/ffmpeg_encoder.cc:1](../src/codec/ffmpeg_encoder.cc#L1)）。
RustDesk 的 VRAM 路径同样零拷贝（`set_output_texture(encoder.input_texture())`），
HWRAM 路径则通过 staging 纹理把画面读回内存。

效果判断：硬件编码两条路径都避免了 CPU 往返；chast 没有额外优势，但
也没有在 FFmpeg 厂商路径上退化成 CPU 编码。RustDesk 的 HWRAM 回退会
多一次 GPU→CPU 拷贝，换来对更多外部编码器的兼容。

## 4. 编码协商与新观看者接入

RustDesk 的编码格式是进程级全局值 `ENCODE_CODEC_FORMAT`
（[codec.rs:43](../../rustdesk-ref/libs/scrap/src/common/codec.rs#L43)），
由所有观看者能力的交集和偏好多数票决定
（[codec.rs:171](../../rustdesk-ref/libs/scrap/src/common/codec.rs#L171)）。
问题在接入路径：`handle_one_frame` 每帧检查
`sp.snapshot(|sps| if sps.has_subscribes() { bail!("SWITCH") })`
（[video_service.rs:1149](../../rustdesk-ref/src/server/video_service.rs#L1149)）。
`snapshot` 只在存在**新订阅**时回调，而回调里的 `has_subscribes()` 检查
的是已有的 `subscribes`（[service.rs:349](../../rustdesk-ref/src/server/service.rs#L349)）。
因此，**已有观看者时，任何新观看者加入都会让该显示的视频服务退出并重建
编码器**，即使协商结果没有变化；现有观看者会经历一次流重启/等待 IDR。
首个观看者加入时 `subscribes` 为空，不会触发这次重启。

chast 把协商做成一个显式事务（[src/host/desktop_stream.cc:1056](../src/host/desktop_stream.cc#L1056)）：
先对所有活跃显示的 Source 执行 `prepare_codec`，为候选编码器实际
编码一帧并检查是否产出可用 IDR（[desktop_stream.cc:472](../src/host/desktop_stream.cc#L472)），
全部通过后才逐源 `commit_codec`；任一显示失败则清空候选、保留原流。
新观看者只有在全局交集/偏好导致格式变化时才触发重建，同一格式下
直接增加订阅。`capture_displays` 的新显示器预订阅也是同样的
“先建好、等 3 s 内出帧、失败回滚”模型
（[src/host/host_server.cc:4164](../src/host/host_server.cc#L4164)）。

效果判断：这是本轮对照中差距最明确的一项。RustDesk 用“重启换正确性”，
实现简单、所有观看者天然同步，但多人陆续加入时会反复打断现有会话；
chast 用“先证明再提交”避免打断，代价是协商代码复杂、失败路径需要
更多测试。chast 侧已有 `chast-host-parity-test` 覆盖跨屏多人投票、
退出恢复和硬件双观看者，但还没有与 RustDesk 的同场景主观对比。

## 5. 帧节奏、背压与恢复

RustDesk 的每条连接有一个**无界**视频通道
（`mpsc::unbounded_channel`，[connection.rs:440](../../rustdesk-ref/src/server/connection.rs#L440)；
`Sender = UnboundedSender<...>`，[connection.rs:72](../../rustdesk-ref/src/server/connection.rs#L72)）。
视频服务发送一帧后，在 [video_service.rs:875](../../rustdesk-ref/src/server/video_service.rs#L875)
最多等 3 s，直到所有 `send_conn_ids` 都从各自通道取走该帧才继续；
每 300 ms 轮询一次。这等价于“按最慢观看者的取帧速度节流”。通道本身
无界，但每帧等待把常态深度压得很小；如果某连接长期不取，队列仍会
随时间增长。连接任务在 `stream.send()` 上等待时，同一 `select!` 里的
控制消息也无法被处理（[connection.rs:934](../../rustdesk-ref/src/server/connection.rs#L934)）。

chast 的做法不同（[src/host/desktop_stream.cc:515](../src/host/desktop_stream.cc#L515)）：

- 每个订阅有 60 帧 / 8 MiB 的独立队列；溢出时清空该订阅、标记
  `waiting_idr` 并要求新 IDR，不把旧参考帧继续塞给慢观看者。
- 所有订阅都满时，`pump()` 直接暂停采集和编码
  （[desktop_stream.cc:722](../src/host/desktop_stream.cc#L722)），
  避免为必然被丢弃的帧做无效编码，也不反复制造 IDR。
- 只要还有一个订阅有空间，其他观看者继续出帧，慢连接不拖住快连接。
- 视频确认（v52 起的 `VideoReceipt`）把源队列、网络和查看端取帧
  时间纳入延迟样本；控制流与视频流在 QUIC 上分离，视频积压不会
  阻塞输入/剪贴板/探测。

效果判断：chast 的隔离性、内存上界和控制面时延明显更好；RustDesk 的
同步等待能保证“不丢帧、所有观看者节奏一致”，但一个慢连接会拖慢
整屏，且新观看者会触发重启。需要说明的是，chast 的“慢观看者丢帧等
IDR”是主动策略，画质上会出现慢端短暂停格；这是用连续性换隔离性，
不是无代价的。两端都没有在真实双机、多观看者、弱网叠加场景下做
同条件对比，因此这里只判定结构差异，不判定端到端体感。

## 6. QoS 与自适应

chast 的 [src/host/video_qos.h](../src/host/video_qos.h) /
[src/host/video_qos.cc](../src/host/video_qos.cc) 是全主机共享的
控制器，跨显示使用同一份策略，规则移植自 RustDesk `video_qos.rs`：
High/Balanced/Smooth 比例 1.5/0.67/0.5；延迟阈值 50/100/150/200/300/600 ms；
比例阶梯 0.95/0.9/0.7/0.85/0.5/0.8；上调 15/10/5%，单次最多 +150 kbps；
启动加速、探测超时降帧、恢复守卫和画质票序都与参考一致。chast 额外
把 `VideoReceipt` 的视频待收年龄和“所有队列满”作为背压证据送入策略，
并在静态刷新帧上不计入动态画面窗口（见第 7 节）。

RustDesk 的 `VIDEO_QOS` 同样是全局的，按 service name 维护每个显示
的编码计数；FPS/码率调整来自 `user_network_delay`、TestDelay 和每帧
取帧等待。它也有查看端确认：能力协商后 `video_ack_required` 置位，
[connection.rs:3598](../../rustdesk-ref/src/server/connection.rs#L3598)
的 `VideoReceived` 会调用 `notify_video_frame_fetched_by_conn_id`；
未声明该能力的查看端退化为“从通道取走即视为收到”
（[connection.rs:934](../../rustdesk-ref/src/server/connection.rs#L934)）。
因此 chast 的 `VideoReceipt` 不是结构上的新机制，差别在于 chast 从 v52
起默认使用它，并把源队列排空和“所有队列满”一起送进策略。两端的 1 s
探测/采样节流也已对齐，避免把 2 Hz 样本当成 1 Hz 使用
（见 `docs/shared-video-qos-rustdesk-2026-11-08.md`）。

效果判断：策略层基本持平，chast 的证据更多、背压更早，但也更复杂。
chast 侧有确定性测试（`chast-video-qos-test` 110 项）和真实 netem
回环数据：在 1000 kbps 瓶颈下目标可降到 1000 kbps 并回升、访问单元
全部解码；这些数据不能直接外推到 RustDesk，因为两端控制器虽同源，
编码器、传输和确认时点不同。`docs/weak-network-acceptance.md` 中
2026-10-22/27 的 8 s→2.1–3.3 s 帧龄数据属于已删除的旧控制器，不能
当作当前 chast 的性能指标；当前口径只有该文档 2026-11-11 之后的
`quic_weak_network` 结果。

## 7. 静止桌面刷新与关键帧

chast 的 `static_refresh_due`（[desktop_stream.cc:299](../src/host/desktop_stream.cc#L299)）
按编码器、厂商后端和画质比例给出刷新预算：VP9 40/20/20/20，H.264/HEVC
区分 NVENC/QSV/AMF/其他，AV1 固定 0；间隔取 `max(1/fps, 1/30)`，
VP9 与第一帧至少 100 ms；VP9 允许刷新失败，其他编码器连续 3 次失败
停止，累计 100 次无输出停止。刷新帧发布前清除
`nv12_updated/desktop_updated`，因此不会伪装成新的桌面变化，也不会
进入共享 QoS 的动态画面计数。

RustDesk 固定基线没有这个模块，只有 WouldBlock 时最多 10 次重复编码；
关键帧策略是“录制时 240 帧间隔，否则不定时”（
[video_service.rs:981](../../rustdesk-ref/src/server/video_service.rs#L981)），
新观看者通过 `SWITCH` 重建来获得首个 IDR。chast 则通过
`StreamRecorder::needs_keyframe`、订阅等待 IDR 和显式
`DisplayKeyframeRequest` 驱动关键帧，录制与在线观看互不牵制。

效果判断：chast 的静止刷新更接近新版本 RustDesk 的行为，能在长时间
静止画面上维持解码器参考链，同时避免把刷新帧误算成运动；代价是
每个编码器/后端需要一张预算表，本机 Intel GPU 只验证了“可解码、
继续取帧”，没有跨机画质对比。RustDesk 基线更简单，但静止画面在
长时间无 IDR 时依赖编码器自身行为，且新观看者必须重启流。

## 8. 光标

chast 在采集线程里逐帧读取指针位置/形状，按显示保存最新 sprite，
通过 `notify_cursor()` 直接投递给每个订阅；协议层有 `CursorUpdate`/
`DisplayCursorUpdate`，支持坐标缩放的 `coalesce_cursor_update`，
最大 sprite 512 px，“显示我的光标”覆盖层使用逐像素 alpha 的
`UpdateLayeredWindow`（[src/win/show_cursor.cc](../src/win/show_cursor.cc)）。
`docs/pointer-acceptance-2026-11-11.md` 记录：旧实现留下 32×32 黑色
方块，修复后覆盖层显示/隐藏两张截图在窗口外采样点逐像素相同
（maxdiff=0），仅箭头填充由背景色变为白色；512 px 真实缩放仍未目视验收。

RustDesk 有独立的光标服务、`capture_cursor_embedded` 选项和更成熟的
白板（彩色光标、用户名、点击涟漪）。本机基线中它也区分“仅查看”和
“控制”两种光标语义。v71 起 chast 的覆盖层同样按查看端设备名取色、
绘制姓名并在点击处播放波纹，协议增加按下边沿（只在可见时合法）。

效果判断：光标正确性和“指针更新不被视频背压阻塞”chast 已经达标，
32×32 缺陷有像素级证据；协作光标的产品化在 v71 后已与 RustDesk
的白板光标对齐到颜色/姓名/点击反馈，自由绘制标注仍未实现。512 px
与隐藏指针的 OS 级场景仍需双机/真实前台窗口验收。

## 9. 音频

两端都是单一音频服务向多个连接扇出，Opus 48 kHz、10 ms 帧。
RustDesk 用 magnum_opus `LowDelay`（[audio_service.rs:103](../../rustdesk-ref/src/server/audio_service.rs#L103)），
10 ms 数据块（[audio_service.rs:22](../../rustdesk-ref/src/server/audio_service.rs#L22)），
并对连续约 8 s 的静音做零门，停止发送直到出现非零样本
（[audio_service.rs:469](../../rustdesk-ref/src/server/audio_service.rs#L469)）。
它支持“语音通话输入设备”热切换和 `audio-input` 选项。

chast 的 [src/host/shared_loopback.cc](../src/host/shared_loopback.cc)
把一路 WASAPI loopback 分发给每个订阅，每个订阅 25 包队列、溢出丢最旧；
Opus 固定 48 kHz（[src/audio/opus_codec.cc:31](../src/audio/opus_codec.cc#L31)），
loopback 默认 160 kbps、其它 128 kbps、麦克风 32 kbps；loopback 有静音门
（默认 2000 ms，[include/chast/audio.h:250](../include/chast/audio.h#L250)），
麦克风侧用 SpeexDSP 做 AEC/AGC/NS（[src/audio/audio_dsp.cc:37](../src/audio/audio_dsp.cc#L37)），
AEC 参考来自同一路 loopback。协议 v35 起音频可走 QUIC DATAGRAM，
解码端用 Opus PLC 补最多 5 帧（100 ms）。主机语音通话走单独的通话
采集与 `send_voice_audio`，设备配置来自 `settings_.voice_microphone`；
`WasapiMicrophoneCapture` 在设备消失时按原 id 重试并上报 recovering，
未指定设备时跟随默认通信设备。

效果判断：chast 在发送侧 DSP、共享扇出和不可靠音频传输上更完整。
`docs/weak-network-acceptance.md` 的实测显示，在同一 1 Mbps/3% 丢包档，
DATAGRAM 音频相对可靠流的平均迟到从 −882 ms 降到约 −100 ms；PLC 让
每个前跳缺口都有输出。RustDesk 基线未在本地源码中找到 AEC/NS 实现、
没有 DATAGRAM 音频，静音门也更粗（8 s）。两端的音频设备选择/切换
细节（主机语音通话、输入设备热切换）都没有做同场景实测，不能仅凭
代码量判定优劣。

## 10. 主机侧截图

RustDesk 的查看端可以发 `ScreenshotRequest`，主机在视频服务的采集循环里
取出**原始帧**转 RGBA，再在后台线程编码/发送
（[video_service.rs:728](../../rustdesk-ref/src/server/video_service.rs#L728)、
`handle_screenshot` 在 [video_service.rs:1392](../../rustdesk-ref/src/server/video_service.rs#L1392)）。
如果当前是 VRAM 路径，它会临时关闭 VRAM 并 `SWITCH` 重建，拿到像素
缓冲后再恢复。

chast 自 v70 起有同方向的主机请求：查看端发 `ScreenshotRequest`，
主机在 [src/host/desktop_stream.cc](../src/host/desktop_stream.cc) 的
`DesktopStreamPool::screenshot` 里经显示服务线程把该流保留的 BGRA 帧拷到
CPU，WIC 编码 PNG 后按 `kScreenshotChunkBytes`（128 KiB）分片走控制流
（`src/host/host_server.cc` 的 `pump_screenshot`）。查看端
`ViewerEngine::ProcessScreenshot`（`app/windows/viewer/viewer_engine.cc`）
收齐分片后写文件；主机拒绝、该流不可用或 10 s 内没有答完时，回退到旧的
“读最新解码帧写 PNG”路径。

效果判断：RustDesk 在 VRAM 路径上会临时关闭 VRAM 并重建编码器，拿到原始
像素但打断视频；chast 的拷贝发生在采集服务线程持有的帧副本上，不关闭
VRAM、不触碰编码器，视频流继续。代价是主机侧多一次整屏 CPU 拷贝和 PNG
编码（1080p 单机回环实测约 250 KiB PNG），协议上界 24 MiB；截图只覆盖
已订阅、旋转为 0 的桌面流（带旋转的帧被 `screenshot` 拒绝），因此
“旋屏原始帧”仍是 RustDesk 路径覆盖更全的场景。

## 11. 录制

RustDesk 的 Recorder 在服务端接收**已编码消息**并写入文件
（[video_service.rs:1042](../../rustdesk-ref/src/server/video_service.rs#L1042)），
录制开启时把关键帧间隔设为 240 帧；可选录后上传。

chast 的 [src/host/stream_recorder.cc](../src/host/stream_recorder.cc)
同样直写编码包，保持无限 P 帧、按需 IDR；队列上限 32 MiB/120 帧，按 10 分钟分段，
文件名按 source 类型（display/merged/camera）+ 哈希生成，支持多源并发
录制。关键帧由 `needs_keyframe` 驱动，不改变在线流的常态参数。

效果判断：两者的核心模型一致（编码包直写、零二次编码），chast 的
分段和多源标注更明确，RustDesk 的录制/上传链路更完整。录制画质和
码率都继承在线编码设置，因此“录制是否足够清晰”最终取决于发送端
码率策略，而不是录制器本身。

## 12. 多显示器与摄像头

RustDesk 每块屏（或摄像头）一个 `VideoService`，帧头带 `display` 索引，
全局编码格式与 QoS 共享；查看端为每块屏维护解码线程和截图上下文。
没有主机侧合并采集。

chast 每块屏一个 Source，`CaptureDisplays` 请求支持 add/sub/set 语义，
拓扑变化按 300 ms 节奏检查；新屏先预订阅并等待可用帧，失败回滚，
不删除现有订阅（[host_server.cc:4185](../src/host/host_server.cc#L4185)）。
摄像头走同一套流池（[src/host/camera_relay.cc](../src/host/camera_relay.cc)），
每连接 4 包/4 MiB 中继队列。主机侧合并视图让查看端可以用一条流看
所有屏，但有 4096 长边/面积上限，且不能做 CPU 旋转。

效果判断：两端的多屏模型等价，chast 多了合并视图和更细的预订阅回滚，
RustDesk 的“每屏一个服务”更简单、故障隔离更直接。摄像头两端都支持，
chast 的每连接队列更保守，RustDesk 与摄像头相关的移动端缩放/编码分支
更多（本机 Windows 对照不涉及）。

## 13. 其他主机侧能力的结论边界

以下能力不属于视频发送主链路，本文只给结论，不展开实现对比：

- 隐私模式：两端都有。RustDesk 有 MAG/虚拟显示等多种实现；chast 的
  虚拟显示路径包含驱动包签名/目录保护检查
  （[src/host/virtual_display.cc](../src/host/virtual_display.cc)）。
  本机没有真实锁屏/UAC/驱动安装场景，未验收。
- UAC/安全桌面：两端都会在桌面切换时重建采集；本机无法制造真实
  UAC/SAS 场景，未验收。
- 协作光标/白板：v71 起 chast 已有颜色、姓名和点击波纹；自由标注、
  远程打印、录制后上传、切换控制方向 RustDesk 有，chast 没有（见 1.1）。
- 主机语音通话与音频设备选择：两端都有（RustDesk 的 `audio-input`/
  voice-call，chast 的 `voice_microphone` + `send_voice_audio`）；
  设备消失时的实际恢复体感未做对比。

## 14. 效果结论

### chast 结构性占优

1. **新观看者接入不重启现有流**：prepare/prove/commit + 全局交集，
   对比 RustDesk 的“新订阅即 SWITCH”。多人陆续加入的会话连续性更好。
2. **背压隔离**：每订阅有界队列、慢端丢帧等 IDR、全满暂停采集；
   慢连接不拖住其他屏/其他观看者，内存有上界。
3. **控制面不被视频阻塞**：QUIC 控制流与视频流分离；RustDesk 的
   `select!` 会在视频 `send` 等待时排队控制消息。
4. **HDR、合并采集、光标独立采样**：普通 SDR 单屏之外的覆盖面更大；
   32×32 光标缺陷已有像素级修复证据。
5. **音频发送侧更有内容**：SpeexDSP AEC/AGC/NS、静音门、DATAGRAM + PLC；
   netem 实测平均迟到从 −882 ms 改善到约 −100 ms。

### RustDesk 结构性占优

1. **编码格式矩阵更宽**：软件 AV1、VP8、VP9/AV1 4:4:4，对没有 AV1
   硬件的机器和特殊解码端更友好；chast 的硬件优先策略是有意取舍，
   但需要产品层明确“没有共同格式”时的降级话术。
2. **主机侧增值功能**：远程打印、录制后上传、切换控制方向；chast 已有
   主机原始帧截图、本地录制和协作光标（颜色/姓名/波纹），自由标注仍缺。
3. **白板自由绘制**：RustDesk 的标注工具可留下绘制内容；chast 的覆盖层
   只跟随指针，不保留笔迹。
4. **音频设备与移动端覆盖**：RustDesk 的 `audio-input`、voice-call
   设备切换和平台覆盖更成熟；chast 已有主机语音通话与设备恢复路径，
   但只到代码审查，未做同场景设备热切换对比。
5. **实现更简单、路径更少**：没有 HDR/合并/事务协商等额外分支，故障面
   更小；chast 的复杂度需要用测试矩阵守住。

### 持平或基本对齐

- DXGI 采集与 GDI 回退、硬件编码零拷贝、码率/画质公式、QoS 阈值与
  票序、Opus 48 kHz/10 ms、编码包直写录制、多屏独立服务模型。
- 两端都没有在“同一台机器、同一内容、同一观看端”的条件下做
  跨产品画质/延迟 A/B；因此**不能**用本文结论宣称 chast 的端到端
  观感优于 RustDesk，只能说在若干机制上更有优势或更完整。

## 15. 未验证项与下一步

必须人工/双机验证，单机回环不能代替：

1. 同内容 A/B：1080p/4K、H.264/HEVC/AV1/VP9、静态/滚动/视频，
   比较 PSNR/SSIM/VMAF、CPU/GPU 占用、端到端帧龄和码率稳定性。
   chast 目前只有 VDENC vs FFmpeg QSV 的同后端对比，没有 vs RustDesk。
2. 多观看者（≥3）陆续加入/离开：确认 chast 不重启现有流，RustDesk
   的 `SWITCH` 在实际会话中的停顿是否可接受。
3. 真实弱网（Wi-Fi/蜂窝、突发丢包、上下行不对称）：当前 chast 数据
   来自单机 netem；RustDesk 侧未运行，不能直接比较。
4. HDR 显示器目视验收：chast 的 scRGB→SDR 映射、白点设置和失败回退。
5. 512 px 光标、隐藏指针、混合 DPI/负坐标多屏：见
   `docs/pointer-acceptance-2026-11-11.md` 的剩余边界。
6. 隐私模式、UAC/安全桌面、虚拟显示驱动安装与恢复。
7. 主机语音通话、麦克风/扬声器设备热切换，以及 AEC 在真实双向语音下
   的效果。通话中换源已有代码路径（见 1.1），仍需真机拔插与默认设备
   变化验收。
8. 主机原始帧截图的旋屏/多屏场景，以及和 RustDesk 截图的内容、时延对比；
   单屏 1080p 回环已由 `chast-host-control-test` 覆盖。

建议的优先级（按投入产出）：

1. 先做同内容 A/B 画质/延迟基线，把“结构优势”变成可比较的数字；
2. 明确 AV1 无硬件时的降级路径与提示（保持硬件优先策略，不引入软件 AV1）；
3. 远程打印、录制后上传按产品需求取舍；主机截图已按“采集线程旁路取原始帧”
   实现（见第 10 节），剩余验收是旋屏/多屏和与 RustDesk 的同内容对比；
4. 主机语音通话与音频设备热切换做同场景验收（chast 已实现通话中换源，
   见 1.1；设备消失重连和默认通信设备跟随见第 9 节）；协作光标的颜色、
   姓名和点击波纹已于 v71 落地，512 px 光标按产品优先级排期；
5. 用现有 `chast-host-parity-test` 增加“新观看者加入不重启编码器”和
   “慢观看者不拖住其他屏”的断言，把本次对照的结构结论固化为回归。
