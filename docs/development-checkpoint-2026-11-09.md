# 开发交接、后续计划与协作约定（2026-11-09）

本文件记录本次暂停开发时的真实状态，作为下次继续工作的入口。当前源码已包含多轮 Windows 发射端、观看端和连接服务改动；**这是开发检查点，不是发布验收完成版**。VP9 / 4:4:4 已接入主要路径，仍处于验证和收尾阶段。

## 1. 保存范围与基线

- 仓库：`ltpara/ds-chcast`。
- 检查点分支：`codex/development-checkpoint-2026-11-09`；实际提交号以该分支 Git 历史为准。
- 开发起点：`ebd689ed680e4c09c55e0e43dd1e5779f2a8d3c0`（原 `main`）。本次保存后续累积的相关源码、已有回归、依赖锁文件、第三方声明及模块文档。
- 当前应用媒体/控制协议：**v61**。主机、服务工作进程、观看端及原生协议工具需要同步构建和升级。
- RustDesk 比较固定于本地参考提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`，不是对其最新发行版的承诺，也不表示线协议互通。
- 按用户要求先保存进度，后续开发等待再次继续；本次不继续扩展功能或执行真实 Flutter VP9 验收。

阅读顺序：本文 → [VP9 / 4:4:4 实现与 v61 协议](vp9-full-color-2026-11-09.md) → 对应模块文档。旧路线图和对比表保留历史快照；它们的“待开发”“已通过”应结合各自日期理解。

## 2. 已实现的功能

下表的“已实现”表示生产路径已经接入。历史模块验收、当前构建和真实设备效果是不同层次的证据，不应互相替代。

| 模块 | 当前实现 | 验证范围及后续边界 |
| --- | --- | --- |
| NAT 穿透、存活与恢复 | 独立签名会合、TCP 预打洞及监听、3/6/9 秒重试、并行直连策略、代理/强制中继、连接结果历史；数据链路保活、自动回退及重新查询鉴权 | 历史 Go / Flutter 检查及本机真实 QUIC UDP 黑洞恢复已验证；跨公网 NAT、IPv6、弱网成功率待实测。见 [连接对齐](rustdesk-alignment-2026-11-08.md)、[NAT](nat-traversal-2026-11-07.md)。 |
| 会合服务角色密钥 | 部署级 link/host 角色密钥把查看器连接、主机注册/审计与管理凭据分离；lookup/lookupBatch 与 UDP/TCP punch 只认 link key，注册与主机签名审计只认 host key，管理入口仍只认 server key；角色密钥未配置时回退旧 server-key | Go API/rendezvous 正负向、Flutter 684 项、C++ 主机设置/隧道测试及真实 `chast-server` 回环通过；公网/代理路径与旧单密钥部署的迁移未验证。见 [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md)。 |
| 独立选屏与共享媒体 | 按显示源共享采集编码、每连接独立订阅和有界发送、独立共享系统音源；观看者退出不再决定其他观看者的采集生命周期 | 已有主机会话、订阅与线程归属回归；复杂物理拓扑及跨机拥塞仍待验收。见 [独立选屏](independent-displays-rustdesk-2026-11-08.md)、[多流](independent-streams-rustdesk-2026-11-08.md)。 |
| 全局编码与共享 QoS | 实际解码能力交集、一连接一票、有效 codec 偏好多数票、多人 FPS / 画质、延迟和取帧反馈、积压恢复；2026-11-09 按 RustDesk `video_qos.rs` 重写启动加速、恢复守卫、探测超时 5 FPS 下限与码率阶梯 | 既有模拟媒体及 QUIC 回归覆盖主要规则，QoS 单测 99 项、主机全量场景 0 失败；v61 新增色度协商的当前结果另见下文。见 [编码协商](global-codec-rustdesk-2026-11-08.md)、[QoS](shared-video-qos-rustdesk-2026-11-08.md)。 |
| 多屏观看、光标与拓扑 | 原生逐屏解码、Flutter 逐屏画面和光标、热插拔/重编号恢复、故障隔离；子窗口共享认证连接、解码结果与输入归属 | 单物理屏双 Flutter engine / GPU 已有历史验收；多物理屏、混合 DPI、多 GPU、虚拟/隐私拓扑仍需检查。见 [观看端](multidisplay-viewer-rustdesk-2026-11-08.md)、[光标](multidisplay-cursors-rustdesk-2026-11-08.md)、[拓扑](multidisplay-topology-rustdesk-2026-11-08.md)、[共享窗口](shared-display-windows-2026-11-09.md)。 |
| 软件编码与恢复 | vcpkg OpenH264；Auto 创建失败时转软件 H.264，普通硬件编码错误连续三次后协商恢复；固定硬件后端不降软件，设备移除走重建 | 已有真实 WARP / GDI / QUIC 及接口故障注入证据；真实显卡故障和不同驱动仍待验证。见 [OpenH264](software-encoder-2026-11-08.md)。 |
| 摄像头共享 | 同设备共享视频源，与桌面共同协商；附带摄像头复用连接投票，Camera-only 支持参数修改和资源释放 | 模拟媒体与真实 QUIC 已验证，物理摄像头尚未实测；当前摄像头不提供原始 BGRA，完整色彩协商回退 4:2:0。见 [摄像头](camera-shared-video-2026-11-08.md)。 |
| 连接管理与审批 | 稳定连接 ID、指定对象聊天/发文件/权限/断开；服务认证管道和 Click/Both 审批，旧 worker 目标失效 | 历史管理及会话检查已通过；完整跨用户体验待验收。见 [连接管理](connection-management-2026-11-09.md)、[服务管理](service-connection-management-2026-11-09.md)。 |
| 服务设置与管理器 | 机器配置编辑、停止/合并并发更新/按原状态恢复、机器范围 TOTP；SYSTEM 按交互用户启动隐藏管理器、单实例与异常重启监督 | 临时真实 SYSTEM 服务、SCM 与实际入口已有验证；跨用户、RDP 切换仍需验收。见 [机器设置](service-machine-settings-2026-11-09.md)、[管理器启动](service-manager-startup-2026-11-09.md)。 |
| Windows 会话与防休眠 | WTS/RDP 列举和选择、登录代次校验及防 ID 复用；默认开启按已准入连接类型持有/释放电源请求 | 已有真实 SYSTEM/WTS 查询与 Windows 电源请求验证，完整多人切换待验收。见 [会话选择](windows-session-selection-2026-11-09.md)、[防休眠](session-power-2026-11-09.md)。 |
| 主机录屏 | 默认关闭的异步 MP4 视频留档，同源多人只录一份，重建/定时分段、背压与关键帧恢复；当前代码已扩展 VP9 | H.264/HEVC/AV1 属于历史模块范围；VP9 容器专项通过，真实主机和观看端 VP9 录屏链路仍待验收。见 [主机录屏](host-recording-2026-11-09.md)。 |
| 本地 UAC 与接管 | v60 查看端提权请求、本地 UAC、父子身份校验、内存配置接管、保留证书和认证信息、重连及失败恢复 | 真子进程/QUIC 和长暂停已有验证；实际 UAC、另一管理员账户待验收，SYSTEM 便携辅助服务尚未实现。见 [提权](host-elevation-2026-11-09.md)。 |
| 文件与剪贴板 | v56 大目录游标分页；v57 双向/单向/禁止策略及独立文件剪贴板权限；v58 空目录；v59 Windows 虚拟文件和 IStream 按需范围读取、身份/代次/取消约束 | 已有真实 OLE、QUIC、双向文件/目录验收；Explorer 人工操作、跨网吞吐、其他应用虚拟文件来源待验证或开发。见 [分页](file-pagination-2026-11-09.md)、[方向](clipboard-direction-2026-11-09.md)、[目录](clipboard-directories-2026-11-09.md)、[按需读取](clipboard-lazy-2026-11-09.md)。 |
| 键盘与本地 IME | 主/子窗口物理键盘与本地输入法模式、IMM 候选窗、Unicode、提交/取消、文本与快捷键顺序、注入回环过滤 | 历史 Flutter 51 项、原生 3 项及双 Flutter 窗口/微软拼音/真实 QUIC 验证通过；主机为 dry-run，双机目标应用和其他 IME 仍待验收。见 [输入法](local-keyboard-ime-2026-11-09.md)。 |
| VP9 / 完整色彩 | libvpx VP9 profile 0 / 1，原始 BGRA → I444，解码 BGRA 上传 GPU；v61 能力和偏好、全局色度协商、实际格式展示、录屏元数据与分段 | **实现中、尚未整体验收完成**；具体协议、专项成绩和缺口见 [VP9 / 4:4:4](vp9-full-color-2026-11-09.md)。 |

## 3. 本检查点的验证记录

### 已确认完成的构建和专项

- 最后一次原生集成构建退出码 0：`chast-host`、`chast-service`、`chast-protocol-test`、`chast-protocol-fuzz-test`、`chast-desktop-stream-test`、`chast-host-parity-test`、`chast-display-video-test`、`chast-mp4-recorder-test`、`chast-host-settings-test`。日志：`build/vpx-integration-build3.log`。
- Windows Flutter Release 构建和 bundle 安装成功：`build/vpx-flutter-build3.log`、`build/vpx-flutter-install.log`。仍有既存 LNK4098 / LNK4199 及第三方编译警告。
- 真实 libvpx 编解码、重复帧、动态码率/FPS、关键帧和 GPU 上传专项全部通过。320×180 单像素红蓝条纹的平均 RGB 误差分别为 4:2:0 **88.957**、4:4:4 **1.497**；两种格式各 8 帧，GPU 与 CPU 转换像素逐字节一致。
- VP9 MP4 两种 profile 各 8 个样本全部重新解码，检查实际 `vpcC` 的 profile / 位深 / 色度；同 codec 切换色度生成两个文件。日志：`build/vpx-live-acceptance3.log`。小图专项不证明 1080p 实时性能或 Flutter 最终显示效果。
- VP9 阶段的四个既有 Flutter 文件共 **44 项通过**，`flutter analyze` 曾报告无问题；之后还有菜单文字和状态逻辑调整，所以不记为最终全树回归。日志：`build/vpx-flutter-tests.log`、`build/vpx-flutter-analyze.log`。

### 保存前原生回归

本次复跑结果为 **7/8 通过，共 142.18 秒；CTest 退出码 1**。本机证据保存为 `build/checkpoint-native-regression-2026-11-09.log`。

| 测试 | 本次结果 |
| --- | --- |
| protocol | 通过，0.11 秒 |
| protocol_fuzz | 通过，0.72 秒 |
| host_settings | 失败，0.20 秒；3 条断言，原因见下文 |
| host_parity | 通过，111.93 秒 |
| desktop_stream | 通过，4.82 秒；包括修正后的色度投票关键帧检查 |
| display_video | 通过，0.28 秒 |
| viewer_display_streams | 通过，23.57 秒 |
| mp4_recorder | 通过，0.50 秒 |

`host_settings` 有 3 条失败断言：测试输入仍使用 `codec=vp9` 表示未知格式，并在解析函数中断言拒绝 `vp9`。生产代码当前已经支持该格式。下次应将非法格式样本改为真正未知值，并保留未知值回退/警告检查，同时确认 VP9 的合法读取和保存。本次按暂停保存要求如实记录，没有继续修改功能或测试用例。

### 2026-11-09 恢复环境后的复跑

检查点原环境未随分支提供依赖和验收日志。本机恢复后已安装 `x64-windows-static-md` 的 OpenH264 2.6.0#4、libvpx 1.16.0#1、libyuv 1916，并用 `OPENH264_ROOT` / `VPX_ROOT` 重新配置根构建和 Flutter 构建树后全量重建：

- 根构建 9 个目标退出码 0；Flutter 查看器 Release 构建和 bundle 安装成功，`app.so` 为本机新生成（`pwsh.exe` 缺失只产生一条非致命提示）。
- P0-1 已处理：非法格式样本改为 `codec=vp10`，新增 VP9 解析、写名断言及保存/读取往返。`host_settings` 通过（0.09 秒）。
- 8 项原生回归 **8/8 通过，总耗时 177.96 秒**，`host_parity` 148.13 秒。直接运行 `chast-host-parity-test.exe` 为 0 失败场景；原 120 秒上限在本机必然超时，已调整为 240 秒。
- 证据日志保存在本机 `build/v61-*.log`，不提交；其中 `v61-native-regression2.log` 为本轮全绿记录。
- 仍未完成：真实 VP9 / 4:4:4 双 Flutter 窗口验收、1080p 性能、P0-2 和 P0-3 的异常路径。临时验收程序源码仍不在本机，P0-2 开始前需要重新生成。

### 2026-11-09 P0-2 真实 VP9 / 4:4:4 验收

旧临时验收源码确实不在本机，已在 `build/vpx_acceptance.cc` 重新生成（`build/` 被忽略，不提交；为其添加的 CMake 目标是临时的，提交前移除）。验收程序在进程内启动真实 `HostServer`（回环 QUIC、Click 模式自动审批、VP9、15 fps、quality 1.5、500 ms 关键帧），采集真实 DXGI 桌面上置顶的 720×480 图案窗（1 像素竖条纹 + 移动白点），用两个真实 `ViewerEngine` + 硬件 D3D11 回读 Sink 比较 BGRA 与已知图案的 MAE，并保存 PPM。

- 条纹宽度先按 2 像素实现时，420 的 MAE（1.282）反而优于 444（1.811）：2 像素条纹正好落在 4:2:0 的色度网格上，属于可表示图案，不能区分色度。改为 1 像素条纹后测试才成立。
- 最终结果（`build/vpx-acceptance.log`）：444 的 MAE 为 1.664 / 1.844 / 2.161，420 为 77.668 / 77.647 / 77.646；第二观看端加入、退出以及 A 的 444→420→444 偏好切换均收敛，主机统计 sessions≥2、codec VP9、最终 chroma 444。截图 `build/vpx-acceptance-444.ppm`、`-420.ppm`、`-444-again.ppm` 仅本机保留。
- 真实 Flutter 双窗口冒烟：用隔离的 APPDATA 配置启动两个 `chast_viewer.exe`（`--connect 127.0.0.1:62670`，预置 trust.json 固定证书）。A 以完整色彩偏好加入，主机 `chroma=1`、codec VP9；B 使用默认 420 加入后主机变为 `viewers=2 chroma=0`（持续 72 个一秒采样）；B 退出后主机回到 `viewers=1 chroma=1`（65 个采样），A 日志出现两次流重公告并重建解码器。日志见 `build/vpx-serve.log`、`build/vpx-viewer-444.log`、`build/vpx-viewer-420.log`，截图见 `build/vpx-window-a-444.png`、`build/vpx-window-b-420.png`、`build/vpx-window-a-444-again.png`。
- 负向验证：预置 trust.json 时把指纹抄错一位（“227**9**46386” vs 实际“227**0**46386”），观看端在 `allow_unverified=0` 下被原生层明确拒绝，主机保持 `viewers=0`。这证明固定身份校验按预期工作，不是代码缺陷；随后用正确指纹连接成功。
- 证据边界：Flutter 窗口把 1920×1080 缩放到约 1100×700，1 像素色度细节在该尺寸不可判别，窗口截图只能证明双窗口真实连接、显示和收敛；像素级的 420/444 区分由进程内 GPU 回读完成。两种格式在 Flutter 侧走同一条 BGRA 纹理上传路径。

### 2026-11-09 P0-3 协商与录屏异常路径

**多源 prepare 失败不部分提交**：`tests/desktop_stream_test.cc` 新增 `multi_display_full_color()`。两块显示共享同一 `CodecPeer`，都具备原始 BGRA，先从 VP9 4:4:4 起步；一个 4:2:0 观看端加入后，`CodecGroup::apply` 重新证明并把两块显示一起切到 4:2:0。随后让一块显示在 444 证明时失败、移除限制端，两组仍保持 4:2:0（没有任何一源单独提交）；显式 444 偏好也只能在两块都证明成功后同时提交，最终两块都回到 4:4:4。退出时采集/编码器均在各自服务线程释放，`wrong_thread == 0`。这覆盖了 `apply()` 的“全部 prepare 成功才 commit”和 444→420 回退分支。

**摄像头导致 420 回退**：`tests/host_parity_test.cc` 新增 `shared_camera_full_color()`，走真实 `HostServer` + 回环 QUIC + 确定性媒体边界（`host_media_stub.h`），不依赖真实摄像头。桌面带原始 BGRA、以 VP9 + 4:4:4 能力/偏好连接并收到 4:4:4；同一连接打开只提供 NV12（无 BGRA）的摄像头后，摄像头与桌面都收到 4:2:0，证明摄像头源加入共享 codec 组时整组原子回退、桌面继续可解码；停止摄像头后桌面恢复 4:4:4。既有 `shared_camera_service()` 继续覆盖 Camera-only 会话、camera-only 选项变更、多观看端复用同一摄像头采集/编码器以及权限撤销。证据边界：本机 `Get-CimInstance Win32_PnPEntity` 过滤 `Camera`/`Image` 结果为空，没有物理摄像头，真实 Media Foundation 采集仍需在有摄像头的机器上复验；本次证明的是主机协商、共享组和桌面连续性。

**录屏格式切换后独立成段**：`tests/mp4_recorder_test.cc` 新增 VP9 色度滚动用例。夹具改为 libvpx 真实生成的 64×64 profile 0（4:2:0）与 profile 1（4:4:4）关键帧及其真实 inter 帧——FFmpeg 的 VP9 解析器按码流未压缩头标记关键帧，用关键帧冒充 delta 会得到错误的样本统计。写入序列为 444 关键帧、444 delta、420 delta（跳过）、420 关键帧、420 delta、444 关键帧、444 delta；结果 3 个独立文件，每段 2 个样本、1 个关键帧、时间轴从 0 开始，`skipped() == 1`，各段 vpcC 的 profile/子采样依次为 {1,3}、{0,0}、{1,3}。主机侧 `StreamRecorder` 在 generation/epoch/codec/尺寸变化时关闭并重建 `Mp4Recorder`，色度变化由 `Mp4Recorder` 自身滚动到 `-2`/`-3` 段；观看端 `ViewerEngine::record_picture` 直接使用收到的 `FrameHeader`（含 chroma），因此两端共用同一滚动逻辑。本轮实际运行 `chast-mp4-recorder-test` 与 `chast-desktop-stream-test` 全部通过，`chast-host-parity-test --camera` 的两个场景通过。

**全量回归中的旧断言**：Release 全量构建通过；首轮 ctest 62 项中 61 项通过，`multi_session` 在隐私/主机操作两条断言上失败。检查点提交 `90ace27` 已把这类拒绝文案统一为 `another viewer controls this host`，但 `tests/multi_session_test.cc` 仍断言旧文案 `...owns the shared stream`。这是测试与实现不一致，不是运行时回归；已把两条断言改为当前实现文案，单独复跑 `multi_session checks passed`。修正后的第二轮 Release 全量 ctest 为 62/62 通过、0 失败，总耗时 1016.21 s（`multi_session` 2.40 s、`host_parity` 151.43 s、`quic_loopback` 38.99 s，其余 `quic_*` 多在 36–41 s）。

### 2026-11-09 P0-4 1080p 性能测量

**方法**：真实 `chast-quic serve`（真实 DXGI 采集 + 编码）与 `chast-quic connect`（真实解码）经回环 QUIC UDP 连接，1920×1080、6000 kbps、30 fps、请求 12 秒（serve 运行 20 秒）。本机 8 个逻辑 CPU、Intel HD Graphics 630、Release 构建。CPU 取自两个进程的 `TotalProcessorTime` 差值，占空比按 8 核和连接墙钟折算，包含采集、转换、加密与收发，不只是编解码本身；帧率与码率由接收端统计（帧数 / 实际窗口、字节数 × 8 / 窗口）。“空闲”指桌面上没有合成动画窗口；“运动”指覆盖主屏的 60 Hz 全屏动画窗口。六轮均 digest 一致、`crc-errors=0 decode-errors=0 gaps=0 protocol-errors=0`。证据为本机 `build/p04-summary-*-final.json` 与 `build/p04-{serve,connect}-*-final.log`。

| 场景 | 编码 | 帧数 | 平均 fps | 平均码率 | 编码 CPU | 解码 CPU | 帧龄 avg/max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 空闲 | H.264 4:2:0 | 570 | 28.7 | 5.78 Mbps | 2.66 s（1.6%） | 4.08 s（2.4%） | 7.5 / 52.2 ms |
| 空闲 | VP9 4:2:0 | 568 | 28.5 | 0.30 Mbps | 10.91 s（6.4%） | 4.45 s（2.6%） | 14.4 / 214.4 ms |
| 空闲 | VP9 4:4:4 | 474 | 23.7 | 0.45 Mbps | 16.52 s（9.7%） | 4.66 s（2.7%） | 157.6 / 504.7 ms |
| 运动 | H.264 4:2:0 | 569 | 28.7 | 5.82 Mbps | 2.14 s（1.3%） | 3.41 s（2.0%） | 10.3 / 50.2 ms |
| 运动 | VP9 4:2:0 | 438 | 22.0 | 1.47 Mbps | 33.19 s（19.5%） | 6.06 s（3.6%） | 24.6 / 505.8 ms |
| 运动 | VP9 4:4:4 | 362 | 18.2 | 3.00 Mbps | 50.58 s（29.9%） | 9.06 s（5.4%） | 61.5 / 572.0 ms |

- H.264 走 oneVPL 硬件 CBR（`MFX_RATECONTROL_CBR`），静态桌面也按目标码率持续发送，因此空闲与运动都是约 5.8 Mbps；帧率稳定 30，运动场景编码仅 2.14 s CPU / 20 s。VP9 是 libvpx 软件 CBR，静态内容按需只发 0.30–0.45 Mbps。
- 六轮平均 fps 都低于 30，差额来自 `VideoQos` 有意的会话启动策略：新会话先限 15 fps（`delay.fps.value_or(15)` 与开盘 1 秒上限），再按延迟采样爬到 30。sender 最终 `framerate=30`（VP9 自适应轮次除外）；按 20 秒窗口折算的 28.5–28.7 fps 与 1 秒 15 fps 的暖机损失一致，不是掉帧。
- VP9 1080p 实时编码在本机是 CPU 瓶颈：4:2:0 运动 33.19 s CPU（约 1.67 核，75.8 ms/帧），4:4:4 运动 50.58 s（约 2.54 核，139.7 ms/帧）。延迟上升后 `VideoQos` 把帧率降到 12 和 3，`BitrateController` 把目标降到 5400/5557 kbps，sender 出现 21/24 次流重公告，这正是自适应在两个控制器间生效的证据。解码侧 libvpx 单帧约 13.8 ms（420）和 25.0 ms（444），仍有余量。
- 帧龄在硬件 H.264 运动场景为 10.3 ms 平均 / 50.2 ms 最大；VP9 444 运动为 61.5 / 572 ms，与编码器排队一致，回环 RTT 仅 0.2–0.7 ms，说明延迟来自编码而不是网络。

**测量中发现并修复的限速缺陷**：第一轮调度调整后 30 fps 请求在 1080p 真实桌面上仍只有约 19 fps；临时探针测得 30 fps tick 有约 32% 错过采集窗口。根因有四个，全部在 `DesktopStreamPool::Source`：

1. 编码结束后才把 `due` 设为 `now + interval`，实际周期变成 interval + 采集 + 编码。
2. 条件变量等待被 5 ms 轮询上限截断，每个 tick 额外晚约 2.5 ms。
3. Windows 默认定时器粒度约 15.6 ms，`wait_for` 被向上取整到下一帧。
4. DXGI `AcquireNextFrame` 只给 5 ms 窗口，与 60 Hz 呈现节奏锁相，整段间隔报告无新帧而下一段连来两帧。

修复依次为：`run()` 内 `timeBeginPeriod(1)`/`timeEndPeriod(1)`（`winmm` 已加入链接）；`due += interval` 从本 tick 起算，编码慢于间隔时跳过一拍而不是连发两帧补课；pending tick 时等待到 `due` 而不是每 5 ms 轮询；采集超时改为 `clamp(due - now, 1 ms, interval)`，让采集线程在整个节奏间隔内等待桌面变化。修复后用临时探针确认采集命中率为 100%（探针已移除），同一 1080p 源 20 秒收到 570/569 帧（暖机后稳定 30 fps），digest 一致且无接收错误。

### 2026-11-09 P0-4 HDR、旋转、缩放与逐屏输入

- **HDR 归一化缺陷已修**：`kHdrToSdrPixelShader` 把中间变量命名为 `linear`，而 `linear` 是 HLSL 保留的插值修饰符，整条像素着色器编译失败。改名为 `rgb` 后 `capture_transform`（真实适配器）通过：scRGB 0.18 → sRGB 118、0.5 → 188、1.0 → 255，高光饱和为 255，alpha 255；`sdr_white_level=2000` 时同一输入变为 85 / 137 / 188。输出 BGRA8 目标共享句柄可被编码器导入。边界：使用合成 FP16 纹理验证数学，本机桌面是 SDR，未接真实 HDR 显示器，`DISPLAYCONFIG_ADVANCED_COLOR_INFO` 的端到端路径未实测。
- **旋转**：D3D11 视频处理器 `ROTATE90` 在真实适配器上执行，64×32 → 32×64，四个象限按顺时针映射（左下→左上、左上→右上、右下→左下、右上→右下），输出共享句柄可用于编码器导入。边界：只验证 90°，180°/270° 与真实竖屏显示器未测。
- **缩放与色度**：视频处理器把 64×64 BGRA 缩到 32×32 NV12，BT.601 limited 下红/蓝 luma 为 82/42（期望 81/41），U/V 为 90/240/240/110；24×24 输出保持图像尺寸 24，同时把共享表面按 16 像素对齐到 32×32 供 oneVPL 导入。
- **逐屏输入映射**：Dart `frameToDesktop` 用显示源原点和尺寸把本地帧坐标换成桌面坐标；负原点（主屏左侧）、正原点（右侧）、半像素远离零舍入、位置钳制在显示矩形内而不是落到相邻显示器、源尺寸未知时回退 1×1、切屏后同一帧重新映射，均有断言。本次运行的逐屏相关 Dart 测试 **50 项通过**（`input_mapping` 7、`display_stream_controller` 5、`display_stream_bridge` 3、`session_display_streams` 2、`display_cursor_view` 1、`display_menu` 32）。边界：只证明坐标数学和窗口状态；本机单显示器，未在真实多显示器上注入点击验证。

### 2026-11-09 P0-4 codec 分支、诊断、原始流工具与打包

- **编码器工厂**：VP9 只走软件后端（libvpx）；固定 `vpl`/`ffmpeg` 后端请求 VP9 时明确报错 “VP9 is available through the software backend”，不静默改后端。非 VP9 的 4:4:4 请求报 “this encoder supports 4:2:0 only”。Auto 回退时主机记录 “the encoder fell back: requested X, producing Y”，不把回退后的码流按请求格式公告。
- **解码器工厂**：VP9 → libvpx；`supported_decoder_444_codecs` 只返回 VP9；H.264/HEVC/AV1 的 4:4:4 请求明确拒绝；`supported_decoder_codecs` 对 VP9 无条件可用（软件），对硬件格式逐个 `CheckVideoDecoderFormat(NV12)` 探测。协议 codec wire 仍为 1/2/3/4，未知值在解析处报错。
- **原始流工具**：`chast-decode` 解析器接受 `h264|hevc|av1|vp9`，但运行到分流前对 VP9/AV1 明确报 “has no Annex B framing; this tool replays H.264 and HEVC elementary streams only” 并退出 1；`decode_rejects_vp9` ctest 同时断言非零退出和该文案，防止 VP9 被静默按 Annex-B 回放。`chast-encode` 只接受 h264/hevc/av1，没有 VP9 分支。`chast-quic verify` 对 VP9 在接收时保留真实访问单元边界（`replay_units`），离线重放走 `digest_unit_list`；`digest_saved_stream` 对 VP9/AV1 明确报 “has no Annex B delimiter”，保存的裸 VP9 文件不会被重新切分。
- **依赖与打包**：第 6 节依赖表已按本机 vcpkg 实际版本补齐 triplet。`tools/package-windows.ps1` 在暂存根目录只保留四个产品 EXE（`chast_viewer.exe`、`chast-tunnel.exe`、`chast-host.exe`、`chast-service.exe`），其余 `*.exe`（含临时验收程序）全部删除；脚本通过 PowerShell 语法解析。本轮用真实 Flutter Release 树实际执行了打包：暂存目录与 `chast-0.1.0.zip` 内均只有上述四个 EXE，安装器 `build/package/out/chast-0.1.0-setup.exe` 生成成功（56,689,138 字节，SHA-256 `4e2f65c5…c9a34`）。本机没有 `drivers\usbmmidd_v2`，脚本按预期警告 `--virtual-display` 安装会被拒绝。边界：本轮只验证了打包与内容白名单，没有重新执行安装/修复/卸载流程。
- **最终全量回归**：包含新增用例的 Release ctest 64/64 通过、0 失败，总耗时 1026.44 s（`capture_transform` 0.13 s、`decode_rejects_vp9` 0.16 s、`host_parity` 153.00 s、`quic_loopback` 39.65 s、`quic_weak_network` 29.38 s，其余 `quic_*` 多在 36–41 s）。证据为本机 `build/p04-full-ctest-final.log`。

### 2026-11-09 静止桌面刷新对齐 RustDesk `static_refresh.rs`

- `DesktopStreamPool` 的显示源在 `NoNewFrame` 时按编码器/厂商后端/画质分档刷新缓存画面：VP9 40/20/20/20，H.264 与 HEVC 区分 NVENC/QSV/AMF/其他，AV1 为 0；首次与 VP9 至少 100 ms，其余间隔取 `max(1/fps, 1/30)`。真实桌面变化才重置预算，纯光标采样不续期。
- 刷新帧清除 `nv12_updated`/`desktop_updated`，不参与共享 QoS 的动态画面窗口；刷新失败回退新 IDR，VP9 允许丢帧，其他编码器连续 3 次失败或累计 100 次无输出即停止。
- 传输层把刷新帧作为 best-effort：不占视频待收窗口、不为停滞观看者等待、不进入延迟统计，但受 QUIC 在途发送上限约束。
- 回归中发现并修复多屏重建窗口的 IDR 饥饿：静止刷新以最高 30 fps 续填每订阅队列，连接适配器等待 IDR 时丢弃所有非 IDR，导致请求的关键帧长期不可达。现在发布 IDR 会清空该订阅旧刷新帧，IDR 下一次即可被取走。
- 验证：`chast-desktop-stream-test` 408–410 项检查连续 3 次通过；`chast-host-parity-test` 全量 976–977 项检查连续 3 次 0 失败，`--topology` 连续 4 次通过；`chast-video-qos-test` 通过；真实 Intel GPU 双查看器 `--hardware` 92 项检查 0 失败；Release ctest 64/64 通过（1058.29 秒）。日志 `build/desktop-stream-rerun1..3.log`、`build/parity-final-run1..3.log`、`build/parity-topology-fix-run1..4.log`、`build/video-qos-final.log`、`build/parity-hardware-final.log`、`build/ctest-final.log`。边界：真实 GPU 只验证静止桌面请求 IDR 和连续取帧，未做跨机画质/功耗对比。

### 2026-11-09 按设备会话偏好对齐 RustDesk `PeerConfig`

- `ui.json` 根键继续作为全局默认，新增 `peers.<peerId>` 分区保存单设备覆盖：画质、编码、FPS、全色（`stream_prefs.dart`）及适配、统计、滚轮反向、左右键互换、键盘模式（`view_prefs.dart`）。读取时根键与设备键逐项合并并逐键校验，非法或旧格式设备分区回退到根值，不阻断其他设备。
- 设备键取服务器分配的 host ID；直连使用地址标签（`host:port` / `[ipv6]:port`），与会话上报身份一致。文件浏览器排序/目录与终端字号保留为全局本地面板状态，不随设备变化。
- 独立显示器窗口通过 Dart 方法通道参数 `peerId` → 原生 `open_display_window` 回调 → 子引擎参数 `--display-window <stream> --peer-id <id>` 取得同一设备键，打开时按该设备加载键盘/视图偏好；原生端只做长度上限校验，不需要了解主机 ID 语义。
- 写路径使用既有跨窗口存储锁，设备保存先写共享键再写 `peers.<id>`；根键不被设备保存污染（`view_fit` 等不再落到根），重复保存不会累积旧值。
- 验证：`stream_prefs_test.dart` 与 `view_prefs_test.dart` 共 26 个用例通过（含每设备独立、无覆盖回退、损坏分区逐键回退）；会话窗口/显示流 8 个用例通过，断言键盘模式按 `127.0.0.1:1` 保存且全局默认不变；`flutter analyze` 无问题；`flutter build windows --release` 通过（264.7 s，日志 `build/device-prefs-build.log`，仅有既有宏重定义与 LIBCMT 链接警告）。边界：未做真实双主机切换的人工复核。

### 2026-11-09 进程级剪贴板监听对齐 RustDesk `clipboard_listener`

- 新增 `ClipboardListener`：每进程一个消息窗口 + `AddClipboardFormatListener`，`WM_CLIPBOARDUPDATE` 只推进原子代次，订阅者回调在锁外执行；`Subscribe` 返回订阅 id，首个订阅启动监听线程，最后一个退订时关闭窗口并 join。监听线程意外退出后可在下一次订阅时重建，不会永久失效。
- 查看器原生后端（未注入 `SetClipboardSource`/`SetClipboardFileSource`）订阅监听，读取门由“代次变化或 5 s 兜底”触发；主机在 `request_local_clipboard` 首次订阅，`stop()` 在线程 join 后退订。只有 `Read` 成功提交才推进 seen 代次，提交被拒时下一轮重试，不吞变更也不忙等。合成后端（`config.clipboard_read`/`clipboard_read_files`）保持 500 ms 轮询，既有回环测试语义不变。
- 验证：新增 `chast-clipboard-listener-test` 在私有窗口站/桌面写真实 `CF_UNICODETEXT`，9 项检查覆盖双订阅者共享同一监听、代次前进、第二次变更、部分退订后剩余订阅者继续收到、已退订者不再被调用、最后退订后 `Running()` 为假；ctest `clipboard_listener` 通过（0.27 s），`clipboard_async` 15.78 s 通过，`chast-host-parity-test` 976 项检查 0 失败（日志 `build/host-parity-cliplistener.log`），Release 构建通过（日志 `build/clip-listener-build.log`）。边界：服务模式/非交互窗口站、多窗口/多主机复制的真实 GUI 复核未做；监听不可用时自动退回 500 ms 轮询，不以“事件驱动”宣称覆盖所有环境。

### 2026-11-09 会合服务角色密钥分离（G01）

- 服务端新增 `--link-key`/`--host-key`（必须同时设置 `--server-key`，三者互不相同）；未设置角色密钥时保持旧单密钥语义，管理 key 始终可作为根凭据。`linkAuthorized`/`hostAuthorized`/`serverKeyAuthorized` 分别守护 lookup 与 lookupBatch、注册与主机签名审计、管理入口（console/groups/wake/metrics/审计查询/录制管理），Bearer 比较使用常量时间。UDP 与 TCP punch 的 HMAC 校验接受 link key 或管理 key，拒绝 host key 和错误 key；空 key 只在两者都未配置时允许公开打洞。
- 查看器 `ServerSettings.linkKey`（为空时 `connectionKey` 回退 server key）覆盖单条/批量 lookup 与 `chast-tunnel viewer --link-key-stdin`；设置对话框新增连接密钥字段及英文词条。主机 `HostSettings.host_key` 经 ini、服务模式合并、`chast-tunnel host --host-key`/`--host-key-stdin` 覆盖注册、保活续期与审计转发，托盘连接服务对话框可编辑，且 `--server-key` 仅在未配置 host key 时才作为回退。
- 验证：`go test ./...`（服务端全量）全绿，覆盖角色密钥正/负向、真实 UDP/TCP punch 配对与空 key 回退；角色 key 对 lookup/批量 lookup、注册、主机审计、建号、身份重置/标签、管理录制列表、WOL、组、metrics、console 审计查询的拒绝均有断言。`flutter analyze` 无问题、`flutter test` 684 项通过；`chast-host-settings-test`、`chast-host-tunnel-test` 通过。真实 `chast-server` 回环（三密钥、SQLite、禁用 carrier）：host key 注册并保活就绪、签名审计上传成功且 spool 消费后删除；link key lookup 200 并完成 UDP punch→TCP 直连；host key 被 lookup 拒绝；link key 对注册、主机审计、metrics、groups 均 401；管理 key lookup/metrics/审计查询均 200。
- 边界：回环验收不代表公网 NAT、真实反向代理或代理链路；旧单密钥部署未强制迁移，角色密钥的可信分发与轮换仍由运维负责。

### 证据边界

- `build/` 的日志和一次性验收程序只保留在本机，不随本提交上传；仓库保存上述摘要、既有测试和可执行的恢复命令。
- 旧模块文档的通过记录属于各自阶段，不代表所有历史场景已在 v61 上重测。
- P0-4 的性能是回环、单显示器、本机核显条件下的进程级 CPU 与接收端统计，不代表跨公网或独显机器；真实双 Flutter 窗口 VP9 验收和 P0-3 异常路径已完成，真实摄像头硬件仍未验证。
- 当前 GitHub `security` 工作流的 push 触发仅监听 `main`。新分支推送成功不等于已运行或通过 CI；该工作流也不涵盖原生 Windows GPU 全链路。

## 4. 后续开发顺序与完成标准

| 顺序 | 工作 | 完成标准 |
| --- | --- | --- |
| P0-1 | 收尾 v61 原生和 Flutter 回归 | 修正仍把 VP9 当未知值的设置用例，核查本次未通过项；复用现有测试验证合法/非法 codec-chroma、能力交集、同 codec 色度变更、旧帧拒绝和录屏分段。通过后更新本文，保留失败原因和证据边界。 |
| P0-2 | 真实 VP9 / 4:4:4 会话验收 | 用最终库重链临时验收程序；验证真实 DXGI → VP9 → QUIC → GPU → 双 Flutter 窗口，比较 4:2:0 / 4:4:4 截图；第二观看端加入/退出、能力/偏好改变、444→420→444 均能收敛。 |
| P0-3 | 协商与录屏异常路径 | 验证多源 prepare 失败不部分提交，摄像头/Camera-only 导致 420 回退时桌面仍正确；主机和观看端实际录屏在格式切换后可独立播放，状态与文件一致。 |
| P0-4 | 性能、CLI 和打包收尾 | 测量 1080p 编码/解码 CPU、帧率、延迟和码率；核对缩放、旋转、HDR 归一化及逐屏输入。检查所有 codec 分支、诊断与原始流工具；VP9 不得静默按 Annex-B 回放。补齐可复现依赖说明并排除临时验收 EXE。 |
| P1 | Windows 和真实设备验收 | 实际 UAC、跨管理员用户、多用户 RDP、多物理屏、混合 DPI、多 GPU、摄像头、Explorer 粘贴、其他 IME/布局及真实目标应用；记录设备、步骤、预期、结果和限制。 |
| P1 | 网络与硬件恢复实测 | 跨公网 NAT、IPv6、代理、中继、弱网及不同显卡故障恢复；分别记录成功率、恢复耗时和正常观看者受影响情况。 |
| P2 | 后续产品能力 | SYSTEM 便携辅助服务、软件 AV1 / VP8、远程打印、角色互换、其他平台被控仍未实现；单独拆分设计和验收范围，不标作本次已完成功能。 |

## 5. 后续协作与协议变更约定

1. 先读取本文与对应模块文档，再核对分支、工作区和最新测试结果；保留用户修改，避免重复安装已有依赖或从旧失败日志推断新源码状态。
2. 功能优先，按模块完成实现和必要验证。复用已有测试文件/目标；有针对性的真实验收可放本机 `build/`，不为低收益验证新增大量测试工程。
3. 文档分开记录“已实现”“已构建”“自动化通过”“真实链路通过”“待人工验证”。模拟媒体、故障注入和 dry-run 只证明其覆盖的边界。
4. 协议字段、枚举、长度或能力含义发生不兼容变化时，同步更新版本、两端解析/发送、非法输入处理和文档。当前 v61 不承诺兼容 v60，也不与 RustDesk 线协议互通。
5. 以实际编码结果公告 codec、色度和尺寸。共享协商先验证所有源再提交；能力不支持时明确回退或失败，不把 4:2:0 数据标成完整色彩。
6. 依赖通过 vcpkg 管理，记录 triplet、版本和许可证；遇到网络问题按用户指定使用 `socks5://127.0.0.1:10808`，代理仅用于当前命令/进程。
7. 本机状态、PID、身份密钥、证书、运行日志、构建产物和临时验收程序不提交。Windows 打包前检查 Release 目录，避免把辅助 EXE 当产品发布。
8. 后续提交使用 `codex/` 分支。未经后续任务要求，不将这个检查点视为已发布或已完成整个 RustDesk 对齐工作。

## 6. 下次恢复工作

本机依赖已于 2026-11-09 恢复。CMake 不通过 vcpkg toolchain 自动查找依赖，旧 CMake 缓存没有下列 root 时配置阶段会失败；新机器按本表安装后显式传入全部前缀。版本取自本机 `D:/vcpkg/vcpkg.exe list` 与各包 `vcpkg.spdx.json`，vcpkg registry 参照 `D:/vcpkg` 的提交 `4da8ab8ef9`（2026-03-18），尚未用 manifest baseline 锁定。

| 依赖 | 安装前缀 / triplet | 本机版本 | 用途 |
| --- | --- | --- | --- |
| oneVPL dispatcher | `D:/vpl_install_static`（`VPL_SHARED=OFF` 静态 dispatcher） | 2.16 | Intel 硬件编码；运行时由驱动提供 `libmfxhw64.dll` |
| FFmpeg | `D:/vcpkg/installed/x64-windows-static` | 8.0.1#2 | 硬件编码回退、D3D11VA 解码、MP4 容器 |
| OpenH264 | `D:/vcpkg/installed/x64-windows-static-md` | 2.6.0#4 | 软件 H.264 兜底 |
| libvpx | `D:/vcpkg/installed/x64-windows-static-md` | 1.16.0 | VP9 软件编解码 |
| libyuv | `D:/vcpkg/installed/x64-windows-static-md` | 1916 | BGRA / I444 转换 |
| libjpeg-turbo | `D:/vcpkg/installed/x64-windows-static-md` | 3.1.3 | libyuv 构建依赖 |
| Opus | `D:/vcpkg/installed/x64-windows-static` | 1.5.2#1 | 音频编解码 |
| SpeexDSP | `D:/vcpkg/installed/x64-windows-static` | 1.2.1#1 | 回声消除与预处理 |
| MsQuic | `D:/vcpkg/installed/x64-windows` | 2.4.8#1 | QUIC 传输；构建自动复制 `msquic.dll` |

vcpkg 安装入口（缺包时执行；版本由上述 baseline 决定）：

```powershell
D:/vcpkg/vcpkg.exe install openh264:x64-windows-static-md libvpx:x64-windows-static-md libyuv:x64-windows-static-md
D:/vcpkg/vcpkg.exe install ffmpeg:x64-windows-static opus:x64-windows-static speexdsp:x64-windows-static
D:/vcpkg/vcpkg.exe install msquic:x64-windows
```

oneVPL 不来自 vcpkg：`mfx-dispatch` 是 FFmpeg QSV 的依赖，chast 的编码器直接链接 Intel oneVPL 2.16 的静态 dispatcher，安装到 `D:/vpl_install_static` 并保留 `lib/cmake/VPL`。Flutter 构建树使用同一套 root，但 `FFMPEG_ROOT` 在本机指向 `x64-windows-static`；`docs/vp9-full-color-2026-11-09.md` 记录的录屏专用 FFmpeg 前缀不是通用要求。

在已有工作目录先确认没有未保存修改，再切换检查点分支：

```powershell
git status --short
git switch codex/development-checkpoint-2026-11-09
git log -1 --oneline
```

P0-1、P0-2、P0-3 已完成（P0-2、P0-3 验收见第 3 节；P0-3 已包含全量 ctest）。重新配置后再构建，不使用旧 EXE 的结果冒充当前源码结果：

```powershell
cmake -S . -B build -DVPL_ROOT=D:/vpl_install_static -DFFMPEG_ROOT=D:/vcpkg/installed/x64-windows-static -DOPENH264_ROOT=D:/vcpkg/installed/x64-windows-static-md -DVPX_ROOT=D:/vcpkg/installed/x64-windows-static-md -DOPUS_ROOT=D:/vcpkg/installed/x64-windows-static -DSPEEXDSP_ROOT=D:/vcpkg/installed/x64-windows-static -DMSQUIC_ROOT=D:/vcpkg/installed/x64-windows
cmake --build build --config Release --target chast-protocol-test chast-protocol-fuzz-test chast-host-settings-test chast-host-parity-test chast-desktop-stream-test chast-display-video-test chast-mp4-recorder-test --parallel 4
ctest --test-dir build -C Release -R '^(protocol|protocol_fuzz|host_settings|host_parity|desktop_stream|display_video|viewer_display_streams|mp4_recorder)$' --output-on-failure
```

P0-4 的本机部分已完成：1080p 性能与限速修复、HDR/旋转/缩放、逐屏输入、codec 分支与原始流工具审计、依赖表、打包 EXE 白名单；64/64 全量回归与一次真实打包见第 3 节。仍需在真实 HDR 显示器、多显示器/竖屏和独立显卡机器上复验，并重新执行安装/修复/卸载流程；这些结果不能由本机合成纹理和回环测量替代。P0-2 的临时验收源码在本机 `build/vpx_acceptance.cc`（`build/` 被忽略，不提交；为其添加的 CMake 目标已在提交前移除），新机器需要重新生成并用当前库编译。本轮 P0-4 的采集/动画/汇总脚本同样只在 `build/`，不属于产品。`app/build/windows/x64/runner/Release/` 下即使出现临时验收 EXE 也不属于产品。相关脚本和隔离的 APPDATA 测试配置不是新机器上可直接依赖的仓库文件。

每完成一个模块，就把结果、剩余项和下一步更新到本文或对应模块文档，保证后续接手不依赖聊天记忆。
