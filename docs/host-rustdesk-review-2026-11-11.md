# Windows 发射端与本地 RustDesk 源码功能对照

审核日期：2026-11-11。本轮只审核发射端，即被控 Windows 主机；服务器和接收端另行审核。为确定协议是否闭环，必要时追踪接收端调用，但不据此声称已审核整个接收端。

## 结论与边界

当前发射端已有较完整的远程控制主干：服务运行、多用户会话、多屏共享采集编码、权限控制、硬件及软件编码、音视频、文件与剪贴板、终端、端口转发、隐私模式和录制均有具体实现。不能仅凭 RustDesk 的功能名称把这些列成缺失，也不能因存在同名处理函数便认定体验完全一致。

优先值得补齐的是四项：**独立光标采样、发射端原始截图、OLE 虚拟文件源读取、录制状态驱动周期关键帧**。其次是未认证连接与正式会话名额隔离、便携提权连续性和通话中麦克风切换。AV1 软件 4:4:4、更多虚拟显示驱动、协作光标提示、远程打印及控制方向切换属于后续选项。

这是“主要功能面的源码对照 + 有限构建/测试”，不是逐行证明，也不是全功能实机验收。尤其没有完成双机弱网、多品牌 GPU、真实 UAC/锁屏切换、虚拟显示驱动部署、Office 附件和长时间录制测试。下文“已有”只表示追踪到了实现路径，不代表全部边界场景已通过。

## 比较基线与方法

| 项目 | 本次基线 |
|---|---|
| 我方 | `D:/ds-chcast`，HEAD `0a23a9327ab11597812398cb3858f432d5593b6d`，并包含审核时的未提交修改 |
| RustDesk | `D:/rustdesk`，HEAD `c7f212df2c960fa74bd83d3849a5a7ce7809a047`，提交日期 2026-10-20，Cargo 版本 1.5.1 |
| RustDesk 子模块 | hbb_common `5d52530b636fb296a324260b074780cf559e0093`；当前协议主要在 `libs/base/protos/message.proto` |
| 工作区状态 | 我方审核期间持续出现 v69 文件冲突协商及剪贴板压缩等改动，不能用单个 HEAD 代表全部被读代码；RustDesk 跟踪文件无修改，存在未跟踪 third_party 目录 |
| 旧文档 | 仅作为检索线索，未沿用旧报告的“完全对齐”结论。旧文档中的 RustDesk `7423dce` 不在本次本地仓库中，不能作为当前比较基线 |

审核方式：从发射端入口、配置及协议枚举建立功能表；再追踪权限判断、处理函数、底层实现、退出/恢复路径及关联测试；最后核对 RustDesk 对应功能是否实际有服务端实现。对于“缺失”，同时检查我方协议、发射端和必要的接收端入口，避免把“已有但语义不同”误判为缺失。没有以行数、关键词数量或测试文件名称计算覆盖率。

## 功能覆盖对照

表中的链接指向本地源码。变化频繁的文件优先列文件级入口，详细差异再给行号。

| 功能面 | 我方证据 | RustDesk 证据 | 本轮判断 |
|---|---|---|---|
| 连接准入、密码、人工同意、TOTP、可信设备 | [host_security.cc](D:/ds-chcast/src/host/host_security.cc)、[host_server.cc](D:/ds-chcast/src/host/host_server.cc) | [connection.rs](D:/rustdesk/src/server/connection.rs)、[login_failure_check.rs](D:/rustdesk/src/server/login_failure_check.rs) | 已有；认证前资源预算有进一步提升空间，见 G05 |
| 中央票据、权限续期/撤销、审计 | [access_ticket.cc](D:/ds-chcast/src/host/access_ticket.cc)、[audit_spool.cc](D:/ds-chcast/src/host/audit_spool.cc) | [connection.rs](D:/rustdesk/src/server/connection.rs) | 我方已有独立管理设计；本轮不评价服务器策略分发正确性 |
| Windows 服务、工作进程、登录会话切换 | [service.cc](D:/ds-chcast/host/service.cc)、[worker.cc](D:/ds-chcast/host/worker.cc)、[windows_sessions.cc](D:/ds-chcast/host/windows_sessions.cc) | [windows.rs](D:/rustdesk/src/platform/windows.rs)、[portable_service.rs](D:/rustdesk/src/server/portable_service.rs) | 已有服务模式；便携提权机制部分覆盖，见 G06 |
| DXGI/GDI 采集及恢复 | [dxgi_capturer.cc](D:/ds-chcast/src/capture/dxgi_capturer.cc)、[gdi_capturer.cc](D:/ds-chcast/src/capture/gdi_capturer.cc) | [dxgi/mod.rs](D:/rustdesk/libs/scrap/src/dxgi/mod.rs)、[gdi.rs](D:/rustdesk/libs/scrap/src/dxgi/gdi.rs) | 已有；设备丢失、锁屏及混合 GPU 仍需实机验收 |
| 多屏、显示变化与多订阅者 | [desktop_stream.cc](D:/ds-chcast/src/host/desktop_stream.cc)、[display.cc](D:/ds-chcast/src/capture/display.cc)、[display_subscription_test.cc](D:/ds-chcast/tests/display_subscription_test.cc) | [display_service.rs](D:/rustdesk/src/server/display_service.rs)、[video_service.rs](D:/rustdesk/src/server/video_service.rs) | 已有共享流及显示订阅；不能把多显示器支持列为缺口 |
| GPU 编码、CPU 回退、编解码能力协商 | [encoder_factory.cc](D:/ds-chcast/src/codec/encoder_factory.cc)、[encoder.h](D:/ds-chcast/include/chast/encoder.h) | [codec.rs](D:/rustdesk/libs/scrap/src/common/codec.rs)、[aom.rs](D:/rustdesk/libs/scrap/src/common/aom.rs) | 已有 VPL/FFmpeg 厂商硬编和 OpenH264/VP9 软件路径；软件 AV1/AV1 4:4:4 有差异，见 G08 |
| 带宽自适应、静态刷新、慢订阅者处理 | [video_qos.cc](D:/ds-chcast/src/host/video_qos.cc)、[desktop_stream.cc](D:/ds-chcast/src/host/desktop_stream.cc) | [video_qos.rs](D:/rustdesk/src/server/video_qos.rs)、[static_refresh.rs](D:/rustdesk/src/server/video_service/static_refresh.rs) | 已有；本轮 video_qos 测试通过，不等于双机弱网体验已验证 |
| HDR 桌面显示 | [hdr_pass.cc](D:/ds-chcast/src/capture/hdr_pass.cc) | [hdr.rs](D:/rustdesk/libs/scrap/src/dxgi/hdr.rs:1) | 都存在 HDR 桌面到 SDR 的处理；不能据此宣称任一方支持端到端 HDR |
| 光标形状和位置 | [desktop_stream.cc](D:/ds-chcast/src/host/desktop_stream.cc:711)、[cursor.cc](D:/ds-chcast/src/capture/cursor.cc) | [input_service.rs](D:/rustdesk/src/server/input_service.rs:330) | 已有独立发送通道，但采样仍跟随视频节奏，见 G01 |
| 键鼠、Unicode、触控、笔、释放按键 | [input.cc](D:/ds-chcast/src/win/input.cc)、[protocol.h](D:/ds-chcast/include/chast/protocol.h) | [input_service.rs](D:/rustdesk/src/server/input_service.rs) | 有实现路径；IME、布局切换、触控实机组合未完整验证 |
| SAS、锁屏、重启、阻止本地输入 | [sas.cc](D:/ds-chcast/src/win/sas.cc)、[host_server.cc](D:/ds-chcast/src/host/host_server.cc)、[input.cc](D:/ds-chcast/src/win/input.cc) | [connection.rs](D:/rustdesk/src/server/connection.rs)、[input_service.rs](D:/rustdesk/src/server/input_service.rs) | 已有；依赖权限、服务与桌面状态，不能用便携提权差异否定全部能力 |
| 系统音频、Opus、回声处理 | [shared_loopback.cc](D:/ds-chcast/src/host/shared_loopback.cc)、[audio_dsp.cc](D:/ds-chcast/src/audio/audio_dsp.cc) | [audio_service.rs](D:/rustdesk/src/server/audio_service.rs) | 已有；真实声卡与回声效果待验证 |
| 双向语音及麦克风选择 | [host_server.cc](D:/ds-chcast/src/host/host_server.cc)、[main.cc](D:/ds-chcast/host/main.cc:3394) | [audio_service.rs](D:/rustdesk/src/server/audio_service.rs:51) | 已有设备选择；通话中换源未闭环，见 G07 |
| 摄像头 | [camera_relay.cc](D:/ds-chcast/src/host/camera_relay.cc)、[camera_capturer.cc](D:/ds-chcast/src/capture/camera_capturer.cc) | [video_service.rs](D:/rustdesk/src/server/video_service.rs) | 有枚举、权限与媒体发送路径；设备兼容性未作横向实测 |
| 文本、HTML、RTF、图像剪贴板 | [clipboard.cc](D:/ds-chcast/src/clipboard/clipboard.cc)、[clipboard_win.cc](D:/ds-chcast/src/clipboard/clipboard_win.cc) | [clipboard_service.rs](D:/rustdesk/src/server/clipboard_service.rs)、[wf_cliprdr.c](D:/rustdesk/libs/clipboard/src/windows/wf_cliprdr.c) | 已有多格式路径；新增压缩改动仍在进行 |
| 文件剪贴板、延迟读取及虚拟发布 | [clipboard_virtual.cc](D:/ds-chcast/src/clipboard/clipboard_virtual.cc)、[clipboard_local.cc](D:/ds-chcast/src/clipboard/clipboard_local.cc) | [wf_cliprdr.c](D:/rustdesk/libs/clipboard/src/windows/wf_cliprdr.c:3757) | 接收/发布侧已有虚拟文件；从第三方应用读取虚拟源仍缺，见 G03 |
| 文件浏览、分享目录、隐藏项、分页、续传 | [file_transfer.cc](D:/ds-chcast/src/clipboard/file_transfer.cc)、[host_server.cc](D:/ds-chcast/src/host/host_server.cc) | [connection.rs](D:/rustdesk/src/server/connection.rs) | 已有；v69 覆盖冲突协商正在修改，暂不判定完成或缺失 |
| 发射端原始截图 | [viewer_engine.cc](D:/ds-chcast/app/windows/viewer/viewer_engine.cc) | [connection.rs](D:/rustdesk/src/server/connection.rs:4097)、[video_service.rs](D:/rustdesk/src/server/video_service.rs:1502) | 我方现有为接收画面截图，缺少发射端原始像素请求，见 G02 |
| 接收端录制与发射端录制 | [stream_recorder.cc](D:/ds-chcast/src/host/stream_recorder.cc)、[viewer_engine.cc](D:/ds-chcast/app/windows/viewer/viewer_engine.cc) | [video_service.rs](D:/rustdesk/src/server/video_service.rs:1067)、[connection.rs](D:/rustdesk/src/server/connection.rs:4016) | 两种录制已有；录制状态与周期关键帧协同可提升，见 G04 |
| ConPTY 终端及持久会话 | [terminal_relay.cc](D:/ds-chcast/src/host/terminal_relay.cc) | [terminal_service.rs](D:/rustdesk/src/server/terminal_service.rs) | 已有；没有据此声称所有用户令牌/退出场景都已验证 |
| TCP 端口转发 | [port_forward_relay.cc](D:/ds-chcast/src/host/port_forward_relay.cc) | [port_forward_mux.rs](D:/rustdesk/src/server/port_forward_mux.rs) | 已有；本轮不展开服务器中继实现 |
| 虚拟显示、无头及隐私恢复 | [virtual_display.cc](D:/ds-chcast/src/host/virtual_display.cc)、[display_privacy.cc](D:/ds-chcast/src/host/display_privacy.cc) | [virtual_display_manager.rs](D:/rustdesk/src/virtual_display_manager.rs)、[win_virtual_display.rs](D:/rustdesk/src/privacy_mode/win_virtual_display.rs) | 已有；驱动供应与多 GPU 实测尚需补齐，见 G09 |
| 防休眠、壁纸移除及恢复 | [power_request.cc](D:/ds-chcast/src/win/power_request.cc)、[wallpaper.cc](D:/ds-chcast/src/win/wallpaper.cc) | [connection.rs](D:/rustdesk/src/server/connection.rs)、[windows.rs](D:/rustdesk/src/platform/windows.rs) | 已有；异常退出恢复仍应纳入验收 |
| 协作光标提示 | [show_cursor.cc](D:/ds-chcast/src/win/show_cursor.cc)、[protocol.h](D:/ds-chcast/include/chast/protocol.h:693) | [whiteboard/mod.rs](D:/rustdesk/src/whiteboard/mod.rs:27)、[whiteboard/windows.rs](D:/rustdesk/src/whiteboard/windows.rs) | 有基础提示；缺颜色、姓名和点击反馈，见 G10 |
| 远程打印、控制方向切换 | [protocol.h](D:/ds-chcast/include/chast/protocol.h) | [printer_service.rs](D:/rustdesk/src/server/printer_service.rs)、[connection.rs](D:/rustdesk/src/server/connection.rs:3982) | 未找到我方对应协议和执行路径；属于产品扩展，见 G10 |

## 具体缺口与可提升点

以下“优先级”是实施顺序建议，不是漏洞严重度。A 为建议首批，B 为下一批，C 为按产品需求选择。

### G01 · A：光标采样与视频帧率、同步编码仍耦合

**证据。** [desktop_stream.cc:711](D:/ds-chcast/src/host/desktop_stream.cc:711) 用编码器帧率安排下一次 pump，随后 capture、[notify_cursor:749](D:/ds-chcast/src/host/desktop_stream.cc:749) 和编码均处于该节奏。发送光标可以绕过视频队列，全部订阅者队列满时也会继续采样，因此不能称作“没有独立光标通道”。但它还没有独立采样调度。RustDesk 的 [new_cursor/new_pos:330](D:/rustdesk/src/server/input_service.rs:330) 分别使用约 33ms 的服务周期。

**影响。** 视频降低到 5fps 时，采样调度间隔约 200ms；同步编码阻塞还会推迟下一轮。即使光标包可以立即发出，也无法弥补尚未采样的移动。这里是静态时序推导，尚未给出实机延迟测量。

**建议。** 增设独立光标位置/形状采样服务，保持视频背压隔离；处理桌面切换、负坐标、多屏缩放、形状缓存和订阅注销。避免为了轮询光标重复完整画面采集。

**验收。** 视频分别限制 5/15/30fps，给编码器注入约 300ms 阻塞，再令所有视频队列满；分别记录光标采样间隔和到达延迟。目标应接近独立约 30Hz 调度，且视频恢复、显示拔插和锁屏切换后无悬挂订阅。已有队列满测试不能替代低帧率和编码阻塞测试。

### G02 · A：缺少发射端原始像素截图请求

**证据。** 我方 [SaveScreenshotFromSink](D:/ds-chcast/app/windows/viewer/viewer_engine.cc) 从解码后的 FrameSink 读取 BGRA 再写 PNG。RustDesk [ScreenshotRequest:4097](D:/rustdesk/src/server/connection.rs:4097) 把请求交给视频服务，[截图处理:1502](D:/rustdesk/src/server/video_service.rs:1502) 从采集像素生成 PNG 并返回。我方没有对应的发射端请求/响应路径。

**影响。** 当前 PNG 保留的是接收到的画面；视频压缩损失、协商后的缩放和画面更新延迟仍会存在。把文件存成 PNG 不会恢复被视频编码丢失的细节。

**建议。** 增加携带请求 ID 和 display ID 的原始截图消息，在发射端权限和显示订阅检查后取得原始像素，异步编码并返回尺寸、格式和错误信息。限制频率、像素数与响应大小；处理中途显示移除、桌面切换及权限撤销。

**验收。** 低码率、降低视频分辨率和静止画面情况下，仍能获得采集分辨率的截图；多屏目标正确，过期请求不返回其他桌面内容。此处不保证 HDR 原始数据或受保护内容可捕获。

### G03 · A：支持虚拟文件发布，但尚不能读取 OLE 虚拟文件源

**证据。** [clipboard_win.cc:330](D:/ds-chcast/src/clipboard/clipboard_win.cc:330) 的文件读取依赖 `CF_HDROP`，即实际文件路径；我方 [clipboard_virtual.cc](D:/ds-chcast/src/clipboard/clipboard_virtual.cc) 已实现 IDataObject、FileGroupDescriptorW 和 IStream 发布。RustDesk [wf_cliprdr.c:3757](D:/rustdesk/libs/clipboard/src/windows/wf_cliprdr.c:3757) 使用 OleGetClipboard、CFSTR_FILECONTENTS、TYMED_ISTREAM 获取应用提供的内容。

**影响。** 只提供虚拟附件、不提供实际文件路径的应用，不能直接经现有源读取路径复制出去。Outlook 一类附件是典型候选场景，但本轮未用具体 Office 版本实测。

**建议。** 为源端增加 FileGroupDescriptorW 枚举和按索引读取 FileContents 的适配器，接入现有延迟分段读取协议。COM 生命周期和线程模型、剪贴板代次、取消、超时、文件名和大小限制都应显式处理。

**验收。** 同时覆盖实际文件路径和仅提供 IStream 的测试源；复制后更换剪贴板、关闭源应用、取消大附件、连续复制同名附件时，不读错代次、不阻塞主线程。不能只测远端发布到资源管理器这一方向。

### G04 · A：录制尚未驱动周期关键帧策略

**证据。** [encoder.h:99](D:/ds-chcast/include/chast/encoder.h:99) 的 GOP 默认 0，即不主动周期 IDR；[stream_recorder.cc:175](D:/ds-chcast/src/host/stream_recorder.cc:175) 在等待 IDR、队列恢复和分段时请求关键帧，分段间隔为 10 分钟。接收端 [录制开启](D:/ds-chcast/app/windows/viewer/viewer_engine.cc) 请求起始关键帧，但没有独立录制状态消息持续影响发射端 GOP。RustDesk [ClientRecordStatus:4016](D:/rustdesk/src/server/connection.rs:4016) 进入共享 QoS，编码器设置按录制状态选择 [Some(240):1115](D:/rustdesk/src/server/video_service.rs:1115)。240 是帧数，不是秒数。

**影响。** 长 GOP 下，录像拖动可能需要从较远的关键帧开始解码，影响定位和恢复体验。这里没有证据表明现有录像必然损坏，也不保证每个硬件编码器始终不自发产生关键帧。

**建议。** 增加接收端录制状态，与发射端本地录制共同决定有限 GOP；对每个共享显示源按录制订阅者计数，最后一个录制者退出后再恢复普通会话策略。具体帧数应结合帧率和带宽测量选择，不应直接照搬 240。

**验收。** 两个接收端分别开始/停止录制，测试共享源、编码器重建和分辨率变化；测量长录像随机 seek 延迟、关键帧间隔、额外码率和分段可播放性。

### G05 · B：认证前连接占用正式观看名额

**证据。** [host_server.cc:1174](D:/ds-chcast/src/host/host_server.cc:1174) 把 `max_viewers` 直接映射为 QUIC `max_clients`；[quic.cc:1290](D:/ds-chcast/src/net/quic.cc:1290) 在 Hello 和人工批准之前就增加 live_clients 并检查上限。单连接模式也先占用 connection。已有 [hello_timeout_ms:146](D:/ds-chcast/include/chast/quic.h:146) 默认 10 秒的超时，因此不是无限挂起。RustDesk [connection.rs:87](D:/rustdesk/src/server/connection.rs:87) 单独管理未认证连接预算，包括全局、每地址、消息大小和等待期。

**影响。** 慢认证或停留在握手中的连接可能临时耗尽很小的观看名额，导致合法连接被拒。属于静态发现的准入公平性问题，本轮没有构造攻击复现，不能据此宣称已证明可远程拒绝服务。

**建议。** 分离 pending 与 authenticated 数量，给 pending 独立的总量、来源配额和期限；认证成功后原子申请正式名额。经过本地 agent 或中继时不能把全部连接视为同一个可信来源，也不能信任客户端自报 IP。

**验收。** 在正式观看名额尚未用满时，制造慢握手、无 Hello、认证失败和取消批准连接，检查正常用户仍可进入、计数正确归还、内存有界。参数按本产品威胁模型设定，不照抄 RustDesk 的等待时间。

### G06 · B：便携提权路径比 RustDesk 窄

**证据。** 我方 [elevation_handoff.cc:127](D:/ds-chcast/host/elevation_handoff.cc:127) 通过本地 UAC 拉起进程，并有认证管道和状态交接。RustDesk [ElevationRequest:3957](D:/rustdesk/src/server/connection.rs:3957) 区分 Direct 和 Logon，后者进入 [portable_service.rs:1096](D:/rustdesk/src/server/portable_service.rs:1096) 的管理员凭据进程启动路径，另有便携服务采集代理。

**差异与建议。** 我方不是没有提权、服务或 SAS，而是缺少该类便携特权代理和远程管理员凭据工作流。若产品要求在未安装服务的设备上连续处理 UAC，可设计受限的特权辅助进程，明确绑定会话、凭据生命周期、进程身份与退出恢复；不宜简单复制凭据传递代码。

**验收。** 标准用户/管理员、允许/拒绝 UAC、锁屏、提权失败及辅助进程崩溃，均应有明确结果并可恢复；真实安全桌面连续性需专门实机验证。

### G07 · B：麦克风选择下次呼叫生效，缺少通话中换源

**证据。** [host/main.cc:3394](D:/ds-chcast/host/main.cc:3394) 保存选择后明确提示“下次语音呼叫生效”；发射端在创建呼叫时读取 voice_microphone。RustDesk [set_voice_call_input_device:51](D:/rustdesk/src/server/audio_service.rs:51) 在设备变化时重启音频服务。这里证明的是 RustDesk 具有服务级换源机制，不代表已验证其所有 UI 场景。

**建议与验收。** 在保持桌面连接和呼叫身份的前提下替换麦克风源，重置回声处理所需状态；测试通话中 USB 设备拔插、默认设备变化、切换到不存在设备以及切回默认设备。已有设备枚举和选择功能应复用。

### G08 · C：可选软件 AV1 与 AV1 4:4:4

**证据。** [encoder.h:48](D:/ds-chcast/include/chast/encoder.h:48) 明确 AV1 由硬件产生，4:4:4 只允许 VP9；[encoder_factory.cc](D:/ds-chcast/src/codec/encoder_factory.cc) 已有 OpenH264/VP9 软件路径。RustDesk [aom.rs:95](D:/rustdesk/libs/scrap/src/common/aom.rs:95) 有 libaom 编码，I444 使用 profile 1，[codec.rs:403](D:/rustdesk/libs/scrap/src/common/codec.rs:403) 包含 VP9/AV1 的 I444 选择。

**建议。** 若低带宽文字清晰度是目标，可试验软件 AV1 4:4:4；先比较相同文字/滚动场景的 CPU、端到端延迟、码率与清晰度，再决定是否默认启用。VP8 属于兼容选项，现阶段优先级较低。不能把这项表述为“我方没有软件回退”或“NVIDIA/AMD 没有硬编”。

### G09 · B/C：虚拟显示的差异在驱动后端与部署成熟度

**证据。** 我方 [make_virtual_display_provider:1615](D:/ds-chcast/src/host/virtual_display.cc:1615) 只构造 UsbmmiddProvider，但已有安装/卸载、包清单与签名检查、权限检查、显示拓扑及恢复逻辑。RustDesk [virtual_display_manager.rs:37](D:/rustdesk/src/virtual_display_manager.rs:37) 分派 RustDesk IDD 和 Amyuni 后端，并有无头和对端请求创建显示的接口。

**建议。** 先验证现有 usbmmidd 包的实际安装与恢复，再决定是否增加第二个驱动供应方。验收应覆盖无物理屏、多 GPU、驱动升级、冲突驱动、隐私开启后异常退出，以及退出时恢复物理屏。源码不能证明当前发布包已经包含可用驱动，也不能在没有包检查证据时称其被“固定哈希/许可”阻断。本轮未安装或卸载任何驱动。

### G10 · C：协作提示与额外产品功能

- **协作光标：** 我方 [ShowCursorHint](D:/ds-chcast/include/chast/protocol.h:693) 只有位置/可见性，显示固定箭头。RustDesk [CustomEvent](D:/rustdesk/src/whiteboard/mod.rs:27) 为 Cursor/Clear/Exit，光标带颜色、按键和文字，Windows 实现含姓名和点击波纹。可补姓名、颜色和点击反馈；不能据此写成“缺少自由绘画白板”。
- **远程打印：** RustDesk [printer_service.rs](D:/rustdesk/src/server/printer_service.rs) 包含动态打印驱动适配器与作业处理；我方未找到对应端到端路径。它依赖驱动/运行时条件，不能声称当前本地 RustDesk 已开箱可用。只有明确打印需求时再立项。
- **切换控制方向：** RustDesk [SwitchSidesRequest:3982](D:/rustdesk/src/server/connection.rs:3982) 拉起反向连接并关闭原会话；我方没有对应协调协议。实现需同时涉及两端、授权与连接恢复，不是给发射端加一个按钮即可。

另有录制磁盘额度、保留策略、错误可见性等工程提升价值，但本轮没有证据证明 RustDesk 在这些方面已完整领先，故不列为“RustDesk 已有、我方缺失”。

## 构建与测试结果

本轮对当前工作区尝试构建以下目标：

```text
cmake --build build-review-clean --config Release --target
  chast-host-security-test chast-host-settings-test chast-clipboard-async-test
  chast-file-transfer-test chast-virtual-display-test chast-host-voice-test
  chast-display-subscription-test chast-host-parity-test chast-video-qos-test
  --parallel 4
```

这是一条命令的换行展示。第一次失败在新增 FileConflict/FileConflictAnswer 声明的类型解析；后续该声明已经变化，重试继续失败在 `src/net/quic.cc` 的新文件冲突发送接口：当时第 9478 行的 `Role::Server`、第 9511 行的 `Role::Client` 报 C2653/C2065。失败发生在编译阶段，不能表述为这些功能测试已经执行后失败，也不能认定这是稳定版本回归。工作区仍在修改，本报告不替其他正在进行的开发作出完成判断。

随后独立构建 `chast-video-qos-test` 成功，执行：

```text
ctest --test-dir build-review-clean -C Release -R '^video_qos$' --output-on-failure
```

结果为 **1/1 通过**。同批尝试构建 `chast-desktop-stream-test` 仍被上述网络库错误阻断，因此没有运行旧二进制并将其冒充为当前源码验证。

此前对其他时间点运行过的桌面流、Go、Flutter 测试不计入这次完整发射端验证。RustDesk 本轮用于源码对照，没有在此机器上重新构建并运行其全部功能。已有的 `host_parity`、`host_voice` 等测试含替身/模拟资源及条件跳过路径，今后即使全绿也需检查实际执行内容，不能单凭名字证明完整产品对齐。

## 补齐“功能完整审核”所需的实机验收

| 场景 | 核心检查 |
|---|---|
| 单屏、多屏、混合 DPI、旋转、拔插 | 源选择、鼠标映射、订阅隔离、重建和新帧恢复 |
| 低帧率、低带宽、抖动、编码阻塞 | 光标延迟、队列有界、音频连续性、恢复时间 |
| Intel/NVIDIA/AMD 与无硬编环境 | 选中实际后端、跨显卡采集、CPU 回退、设备重置 |
| 普通权限、服务、便携提权、锁屏/UAC | 用户令牌、桌面切换、按键释放、权限撤销与重连 |
| 无屏、虚拟屏、隐私屏 | 驱动部署、崩溃/断线恢复、退出还原物理显示 |
| 剪贴板双向真实文件与虚拟附件 | 代次、取消、源应用退出、超时、大文件与主线程响应 |
| 文件同名、续传、权限变化 | v69 冲突行为、内容校验、路径边界、取消后清理 |
| 多人观看与录制 | 共享编码不被单人操作错误重置、长录像 seek、磁盘写失败 |
| 语音、声卡切换、摄像头拔插 | 正确换源、回声处理重置、错误回报和资源回收 |
| 终端与端口转发退出/重连 | 正确身份、权限约束、持久化边界、通道关闭 |

建议第一批按 G01 → G02 → G03 → G04 实施和验收；G05 可并行作为准入稳健性专项，G06/G09 则单独做权限和驱动方案设计。本轮仅新增审核报告，未实现这些建议，也没有修改现有业务代码或旧审核报告。

## 审核快照指纹

以下是 2026-11-11 13:50:58 +08:00 对关键文件读取的 SHA-256，便于发现后续改动导致的结论漂移。它不是整个仓库的原子快照；工作区并行修改仍可能发生。

```text
src/host/desktop_stream.cc
D1C0CFD1C4085C94F5EFD4C31CA91EB2D11FEC63912A1EA1E63B96BD7BCBEBAE
src/host/host_server.cc
631140D35507BC84F4385A76A1781AE3686971A57D915B294B307677E0C3F87A
src/host/stream_recorder.cc
AAB73F486651765F3EC88156F0E6D23BAA6C8266900C7A6451BD2A629F791994
src/clipboard/clipboard_win.cc
7FFBAEA046934A4C34F9ABB91C52072CFC937D33FB2DFAADE43D1AA010BEB12E
include/chast/encoder.h
36159676BA430F25CAC4FE0196705B4AE1A92E7A127CD0CAB3DF97E7F9877A4F
include/chast/protocol.h
BD8DCC88D994B86A9323427360016B2927024F1E6A292941C8A2EAC0F50013BA
src/net/quic.cc
98428F888A3A14EC7F2307171C9E6881BD0BE9E5AAA63CB5B2BFE71E8E261461
app/windows/viewer/viewer_engine.cc
801DCF0DB74A545EA6CCEE5BD70CB63DFD677C8D77EDAAB5499C2C21859331FC
host/main.cc
CF87CA104669A6D2E36229A874C11CF968035965130BF1EEDBAE5DAAE6983446
```
