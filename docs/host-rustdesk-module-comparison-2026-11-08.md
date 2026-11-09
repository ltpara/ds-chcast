# 发射端模块对比：当前工作区与本地 RustDesk

> 2026-11-09 v61 更新：主表已按 `codex/development-checkpoint-2026-11-09`（`ddf75d2` + 工作区 R01–R05 修复，协议 v61）重新核对。此前“首观看者生产、其他人跟随”的结构性差距已由 v50–v55 关闭；v56–v61 补齐分页、剪贴板方向、空目录、按需文件剪贴板、事件监听、防休眠、主机录屏、会话内提权、服务审批和 VP9 4:4:4。下方历史更新记录保留，主表和“剩余差距”为当前结论。

> 静止刷新更新（2026-11-09）：已按 RustDesk `video_service/static_refresh.rs` 接入按编码器/厂商后端/画质分档的静止刷新预算（VP9、H.264、HEVC 表格；AV1 关闭）、首次与 VP9 至少 100 ms、其余不超过 30 fps、桌面变化才重置计数；刷新帧不计入共享 QoS 的动态画面证据，并以不占发送窗口的 best-effort 方式发送。发布 IDR 时清空该订阅旧刷新帧，修复静止桌面关键帧请求被排队刷新掩埋的问题。测试证据见 [共享视频质量](shared-video-qos-rustdesk-2026-11-08.md)。

> 键盘与输入法更新（2026-11-09）：主/子窗口已加入“物理按键 / 本地输入法”模式，本机组合确认后通过已有 TextInput 发送 Unicode；补齐文字与按键顺序、取消和回环注入过滤。真实双 Flutter 窗口/QUIC、微软拼音及快速回车确认通过，相关 Flutter 检查 51 项通过。其他输入法、布局与双机目标应用仍需实测，见 [本机键盘布局与输入法](local-keyboard-ime-2026-11-09.md)。

> 文件管理更新（2026-11-10）：协议 v64 新增 `FileOperation::RemoveTree`，观看端删除文件夹时确认后递归删除整棵树；主机先预检整棵树，发现 reparse point 或只读条目即整体拒绝，再按从深到浅顺序删除。真实 QUIC 回环覆盖父/子文件夹的创建、重命名和递归删除，见 [文件夹递归删除](recursive-remove-2026-11-10.md)。同日 v65 补齐隐藏项：`FileListRequest` 增加 flags 字节，主机在分页前过滤 `FILE_ATTRIBUTE_HIDDEN`，观看端工具栏开关按设备持久化并让递归下载沿用当前视图，见 [隐藏文件浏览选项](hidden-files-2026-11-10.md)。两端都需要同时升级；多共享根/盘符和压缩块仍属 G18。

> 键盘 Legacy 更新（2026-11-10）：协议 v63 新增 `InputKind::LegacyKey`，查看端“传统按键”模式发送本机虚拟键码，主机按 VK 注入由主机布局和输入法解释；修饰键按各自模式释放，锁定键按位图差异同步，切换模式前释放远端按键。协议/主机/查看端自动化与 Windows Release 构建通过；真实双机非 US 布局、AltGr、死键、远端 IME 和安全桌面输入待人工验证，见 [键盘 Legacy 模式](keyboard-legacy-mode-2026-11-10.md)。

> 显示器窗口更新（2026-11-09）：新窗口已改为共享当前认证连接和逐屏解码结果，不再新建查看者会话；支持独立输出解绑、最后帧重放、逐窗口光标与输入归属，主会话结束会关闭子窗口。真实单屏双 Flutter engine/GPU 验收通过，多块物理显示器与混合 DPI 仍待人工验收。见 [同一会话的显示器窗口](shared-display-windows-2026-11-09.md)。

> 提权更新（2026-11-09）：v60 已接入查看端请求、本地 UAC、校验父子进程的内存配置接管和自动重连；保留原证书指纹、密码、可信设备及 TOTP，失败恢复原主机。补回输入桌面周期检查，并修复已通知 UAC 暂停仍被查看端 20 秒停帧期限断开的行为。真实子进程/QUIC 与 22 秒暂停恢复已验收；实际 UAC 和跨管理员账户仍需人工验收，SYSTEM 便携辅助服务尚未实现。见 [本地 UAC 与主机接管](host-elevation-2026-11-09.md)。

> 主机录屏更新（2026-11-09）：已加入默认关闭的本地自动录屏设置，桌面/合并桌面/摄像头复用共享源压缩数据异步写 MP4，同源多人只录一份；支持 H.264、HEVC、AV1，编码器重建与定时分段、积压关键帧恢复及最后订阅退出保存。当前只录视频，机器服务账户和物理摄像头仍需实测，见 [主机自动录屏](host-recording-2026-11-09.md)。下面的模块表保留修复前快照。

> 按需剪贴板更新（2026-11-09）：v59 已将默认 Windows 双向文件剪贴板接入虚拟文件描述与 IStream，粘贴读取时才请求文件范围；复制代次、文件身份、方向权限及取消约束贯穿收发。原生独立窗口站 OLE 验证、实际双向混合/纯目录粘贴和完整回归均通过，主机、服务与观看端已构建，见 [文件剪贴板按需读取](clipboard-lazy-2026-11-09.md)。两端需要同时升级。Explorer 人工操作、跨网吞吐及其他应用虚拟文件来源仍待验收或开发；下面各阶段记录保留其当时范围。

> 空目录更新（2026-11-09）：v58 已支持文件剪贴板双向保留独立及嵌套空目录；纯目录无需文件数据流，零字节文件仍按文件收发。主机续传工作目录移出复制内容。实际 ViewerEngine 上下行混合/纯目录四种场景及完整会话、异步剪贴板回归通过，见 [文件剪贴板保留空目录](clipboard-directories-2026-11-09.md)。两端需要同时升级；粘贴时按需读取见上方 v59 更新。

> 剪贴板更新（2026-11-09）：v57 新增本地双向/单向/禁止策略，覆盖文本、图片和文件剪贴板，并让禁止发送的一端停止本地读取；文件复制已脱离普通文件传输权限。方向专项、完整会话回归和实际观看端双向文件收发通过，见 [剪贴板方向控制](clipboard-direction-2026-11-09.md)。两端需要同时升级；文件剪贴板按需读取随后由 v59 补齐，空目录支持见上方 v58 更新。

> 文件浏览更新（2026-11-09）：v56 已加入大目录游标分页，观看端支持“加载更多”和同页重试，递归下载自动读取后续页。真实 QUIC 验证两页 1030 项，以及 1025 个空目录和 4 个文件完整落盘；主机、服务与 Windows 观看端构建通过，见 [目录分页](file-pagination-2026-11-09.md)。文件剪贴板的按需读取随后由 v59 补齐，下方无分页描述保留为历史快照。

> 防休眠更新（2026-11-09）：新增默认开启的本地 `keep_awake` 设置。已准入桌面连接请求系统及显示器保持唤醒，文件/终端等连接只请求系统；按连接独立释放，待审批连接不持有。三项既有回归全部通过，实际 Windows 电源请求及中英文设置界面已验证，见 [会话防休眠](session-power-2026-11-09.md)。下方“缺少防休眠”的表格保留为历史审核快照。

> Windows/RDP 更新（2026-11-09）：会话选择已支持只有 RDP 用户的主机，并用登录代次识别会话 ID 复用；切换请求绑定来源工作进程和目标登录，快照失败时保留当前工作进程。管理器显示服务状态及本 Windows 会话编号。三项回归和真实 SYSTEM/WTS 查询通过；真实多用户切换仍待验收，见 [Windows 会话选择](windows-session-selection-2026-11-09.md)。

> 管理器启动更新（2026-11-09）：服务已按当前交互用户身份自动启动隐藏管理器，默认托盘在服务安装后让出共享，服务延迟启动/停止时不会误开普通主机；正常退出保持关闭，异常退出退避恢复。真实 SYSTEM→用户进程和实际程序入口已验证，见 [服务管理器启动](service-manager-startup-2026-11-09.md)。完整跨用户/RDP 切换体验仍待验收；会话内提权随后由 v60 补齐。

> 机器设置更新（2026-11-09）：服务管理首页已显示运行中设备 ID、指纹和认证状态；新增管理员机器安全/连接设置编辑器，保存时停止服务、合并并发更新并按原运行状态恢复，TOTP 使用机器范围保护。五项现有回归通过，临时 SYSTEM 服务验证了真实 SCM 保存/重启和失败分支；详情见 [服务机器设置](service-machine-settings-2026-11-09.md)。服务管理器登录启动随后补齐，远程提权由 v60 接入；完整多用户体验仍待验收。下列各阶段说明保留其当时的验证边界。

> 服务管理更新（2026-11-09）：已接入同 Windows 登录会话的认证管道、托盘指定连接管理和 Click/Both 审批；工作进程重启后旧目标失效，发文件按本地用户身份读取。机器配置界面随后补齐，会话内远程提权由 v60 接入。实现、验证及范围见 [服务连接管理](service-connection-management-2026-11-09.md)。

> 连接管理更新（2026-11-09）：已加入稳定连接 ID、按 ID 聊天/文件/权限/断开，以及原生目标选择窗口；新加入或离开的连接不会改变正在操作的对象。已清理顺序会话遗留的本地待发文件，防止交给下一位。当前面板管理托盘进程自己的主机，服务 worker 的跨进程管理与审批仍待开发。见 [按连接管理](connection-management-2026-11-09.md)。

> 摄像头后续更新：同设备已共享采集编码，并与桌面共同协商格式；桌面附带摄像头复用连接投票，Camera-only 支持运行中参数修改、停止释放策略和硬编失败后的软件恢复。验证覆盖模拟媒体与真实 QUIC，本机无物理摄像头，设备实测仍待补齐。详见 [摄像头共享视频](camera-shared-video-2026-11-08.md)。

> 编码兜底更新：已加入通过 vcpkg 安装的 OpenH264，Auto 在 oneVPL/FFmpeg 创建失败后使用软件 H.264；运行中普通硬件编码连续三次失败也会触发全局兼容协商及软件恢复。具体能力、验证与剩余边界见 [软件编码](software-encoder-2026-11-08.md)。以下旧快照中“没有 CPU 编码兜底”的描述已由此更新。

> 后续修复更新：多人独立选屏 F05 已实现，旧发布者/跟随者结构已移除；每屏使用独立视频服务线程，系统音源独立共享。v50 增加实际 GPU 解码能力与同屏交集，v51 接入全局多人 FPS/画质及主机延迟探测，v52 补视频取帧反馈、有限发送窗口与积压恢复，并移除连接线程重复的 FPS 等待。v53 已实现全部桌面连接的解码能力交集、有效编码偏好多数票和 Auto 顺序，并补齐临时切屏订阅的回退清理。当时未等同项（F08 色度能力、多窗口复用同一连接、复杂拓扑恢复等）随后由 v55/v61 继续处理；v55 已接入原生逐屏解码和 Flutter 独立画面，最新范围见 [观看端对照](D:/ds-chcast/docs/multidisplay-viewer-rustdesk-2026-11-08.md)。当前行为与验证见 [独立选屏修复对照](D:/ds-chcast/docs/independent-displays-rustdesk-2026-11-08.md)、[共享质量控制](D:/ds-chcast/docs/shared-video-qos-rustdesk-2026-11-08.md)、[全局编码协商](D:/ds-chcast/docs/global-codec-rustdesk-2026-11-08.md) 和 [同一会话的显示器窗口](D:/ds-chcast/docs/shared-display-windows-2026-11-09.md)。

> v54/v55 进展：已加入单连接多屏选择规则、独立视频流、生产主机调度、逐屏光标与观看端独立解码/呈现；物理屏消失、重编号和几何变化的恢复逻辑及单屏故障隔离已接入，见 [拓扑与恢复对照](D:/ds-chcast/docs/multidisplay-topology-rustdesk-2026-11-08.md)。共享连接的原生多窗口随后由 v55 的逐屏解码/独立画面补齐；虚拟/隐私拓扑完整多选恢复仍未完成。详见 [单连接多屏接入进度](D:/ds-chcast/docs/independent-streams-rustdesk-2026-11-08.md)。

日期：2026-11-08。发射端指 Windows 被控主机，包括托盘、Windows 服务、主机会话、采集编码、媒体发送、输入、剪贴板和附属会话。只在必要时追踪接收端协议配合，不展开账号管理和中央服务。

本项目基线为 `ddf75d2` 加当前工作区 R01–R05 修复及后续 v62–v65 变更，协议 v65；本节主表已按 2026-11-10 当前代码更新。RustDesk 对照使用固定提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`；本机参照仓库为 `D:/rustdesk`，当前检出 `55786aa`，固定提交已浅拉取到同一仓库并用 `git show`/`git grep` 读取，不改变其工作区。本报告不是与最新发行版的比较。

11 月 7 日的旧报告使用另一份固定版本。它的 F01–F04 已修复，旧表格是修复前快照。本轮重新核查当前代码，不把旧问题原样列为现存缺陷，也不混用旧版本行号。

结论：常用单人远控的发射端功能已基本齐全；此前最大的结构差距“一位观看者生产桌面、其他观看者跟随”已经关闭。当前实现按显示源共享采集/编码，每连接独立订阅、独立发送队列与反馈，全局 QoS 汇总全部观看者，支持不同观看者独立选屏和解码能力交集。

与 RustDesk 相比，剩余的是有界产品项和环境验收，而不是主链路缺失：连接 ACL/账号级权限票据（G02/G03）、AV1 4:4:4、远程打印/角色互换、多共享根/压缩块文件管理、虚拟显示驱动和 HDR 端到端。查看端出站自动录制（G11）、文件夹递归删除与隐藏项开关（G18 部分）已于 2026-11-10 补齐。管理权限颗粒度与自动化 API Token（G12）属于中央控制台，不改变发射端矩阵，单独见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md)。真实多屏/混合 DPI、UAC/RDP、物理摄像头、Explorer/Office、公网 NAT 与电源策略仍需按设备记录证据。

## 1. 按模块比较（v63 现状）

“已实现”表示生产路径和自动化回归存在；“部分”表示有路径但仍有明确缺口；“待验”表示代码存在但缺所需设备/网络证据；“缺失”表示当前发射端没有对应完整路径。

| 模块 | 当前发射端 | 本地 RustDesk（7423dce） | 状态与剩余边界 |
| --- | --- | --- | --- |
| 运行与服务 | Win32 托盘、便携模式、SYSTEM 服务及桌面会话 worker；服务按交互用户启动隐藏管理器，支持服务重启/跟随 Windows 会话 | Windows 服务、桌面进程、连接管理器及 IPC | **已实现**。重启、注销再登录、服务恢复和包级测试仍按部署验收。[P01][R01] |
| 登录与审批 | 密码、点击审批、Both、TOTP、签名可信设备、IP/CIDR；服务模式经同登录会话认证管道支持 Click/Both，不再强制永久密码 | CM/IPC 批准流程、按审批模式判断、登录屏幕条件 | **已实现**。真实跨用户审批、RDP 登录屏仍待验收。[P02][R02] |
| 会话内提权 | v60 查看端请求、本地 UAC、父子进程身份/内存配置校验、保留认证信息、自动重连；SYSTEM 便携辅助服务未实现 | `ElevationRequest` 启动 portable service | **已实现/待验**。真实 UAC 与另一管理员账户仍需人工验收。[P03][R03] |
| 多连接与本地管理 | 默认 2、上限 8；稳定连接 ID；按 ID 聊天、发文件、改权限、断开；原生目标选择窗口 | 连接 id；CM 按 `conn_id` 操作 | **已实现**。跨进程服务 worker 管理与完整多用户界面体验待验收。[P04][R04] |
| 全局策略与实时撤权 | 共享策略掩码、版本同步和传输层限制；全局关闭传播到已有及待审批连接；重新启用不自动恢复旧授权 | 按连接权限、全局选项和服务订阅控制 | **已实现**。逐项授权语义仍按 G03 扩展，不宣称全部规则相同。[P05] |
| 单屏、合并屏与多人选屏 | `DesktopStreamPool` 按显示源共享采集/编码；每连接独立订阅、独立发送；发布者离开不再重建他人仍在使用的流；支持拓扑/热插拔恢复 | 按显示器创建视频服务；连接可 add/sub/set 订阅 | **已实现**。真实多物理屏、混合 DPI、多 GPU 和虚拟/隐私拓扑待验；单连接多屏规则已接入。[P06][R05] |
| 编码与能力协商 | Hello 携带 `decoding_codecs`、`decoding_444` 和色度偏好；共享 codec 组取全部订阅连接的交集，偏好多数票，Auto 失败回退 OpenH264；v61 支持 VP9 4:4:4 | `supported_decoding` 与编码协商状态，硬件/软件路径 | **已实现**。AV1 4:4:4 和更多软件格式未提供；异构 GPU 组合待真机验证。[P07][R06] |
| 色彩与文字清晰度 | VP9 profile 1（I444）原始 BGRA → I444，解码后 BGRA 上传 GPU；H.264/HEVC/AV1 仍为 4:2:0；HDR 采集归一为 SDR | 按能力选择 I444；I444 仅用于 VP9/AV1 | **部分对齐**：VP9 4:4:4 与 RustDesk 的 VP9 路径等价；AV1 4:4:4 缺失，H.264/HEVC 两边都不提供 I444。HDR 只到 SDR 输出。[P08][R06][R20] |
| 静止桌面与关键帧 | `NoNewFrame` 时按编码器/后端/画质分档重复编码缓存画面（VP9/H.264/HEVC，AV1 关闭），首次与 VP9 ≥100 ms、其余 ≤30 fps；IDR 入队清空旧刷新帧 | snapshot + 受限重复编码 | **已实现**。真实硬件静态画质收益、各后端预算和长时间功耗待测。[P09][R07] |
| 采集/编码异常恢复 | DXGI `DuplicateOutput` 失败或不可用时切 GDI 上传路径；GPU 重建、桌面切换、缓存失效、硬件后端尝试；普通硬件编码连续失败回退软件 H.264 | DXGI → GDI、编码创建失败 → VP9 等分支 | **已实现（方向一致）**。驱动复位、RDP 进出、热插拔仍待真机复验；GDI 不是零拷贝路径，只作可用性兜底。[P10][R07] |
| 媒体发送与弱网 | 每显示源独立服务线程；全局 `VideoQos` 汇总所有订阅者的延迟/FPS/画质；每连接独立发送队列、积压丢弃和关键帧恢复；不再以生产连接发送成功作为全体广播前提 | 多用户 QoS、帧反馈、有限等待 | **已实现**。极端拥塞/跨机弱网下共享降质规则和恢复时延仍需实测；RustDesk 同样受慢连接影响，不承诺完全隔离。[P11][R08] |
| 系统声音与双向语音 | 注册表级 `SharedLoopback` 系统音源，按会话订阅/退订，各连接独立权限和发送；麦克风上行、呼叫、AEC、静音门 | 共享音频服务，按连接订阅 | **已实现**。真实回声、热插拔、多人争用和音源设备切换待实机验证。[P12][R09] |
| 键鼠、触摸、笔 | HID/扫描码物理映射、Unicode 文本、本地输入法模式、v63 Legacy 虚拟键码注入、相对鼠标、触控/笔、释放屏障、卡键清理、注入回环过滤 | Map / Translate / Legacy 三种模式 | **已实现/待验**：Map、本地输入法（Translate 方向）与 Legacy 三种模式均已接入；非 US 布局、AltGr、死键、其他 IME、安全桌面输入待实测。[P13][R10] |
| 锁屏、SAS、登录与 WTS | SAS、锁屏、重启、BlockInput、登录密码输入、会话列举/切换；跟随会话也消费这些请求；RDP-only 主机和登录代次防 ID 复用 | 对应控制消息、会话/平台服务 | **已实现/待验**。真实多用户切换、UAC/安全桌面进出仍需受控验收；OS 登录目前是登录屏密码输入，不是通用登录自动化。[P14][R02][R03] |
| 隐私与虚拟显示 | 遮罩、BlockInput、隐私拓扑守卫、usbmmidd；通用 `vdd` 明确 unsupported；PLAT-01 虚拟显示被驱动条款/固定哈希阻塞 | 虚拟显示管理、隐私模式与恢复 | **部分/待验**。驱动 provider、崩溃后恢复、多人适用性不同；不能把 provider 名称当可用性证明。[P15][R11] |
| 文本/图片剪贴板 | Unicode、HTML、RTF、DIB；进程级 `WM_CLIPBOARDUPDATE` 监听 + 异步 worker，合成后端 500 ms 兜底；v57 方向策略 | 共享监听、订阅者 | **已实现**。多窗口/多主机复制的真实 GUI、跨会话串扰和阻塞仍待验收。[P16][R12] |
| 文件复制粘贴 | v59 虚拟文件/`IStream` 按需范围读取；v58 空目录；v57 方向；复制内容不立即全量搬运；文件剪贴板脱离普通文件传输权限 | IDataObject/FILECONTENTS 按需内容请求 | **已实现**。Explorer/Office 人工操作、其他虚拟文件来源、跨网吞吐、取消/源变化待验收。[P17][R13] |
| 文件浏览与传输 | v56 游标分页；共享根、reparse/路径防护；上传下载、续传、取消、目录操作；v64 文件夹递归删除（预检 reparse/只读后自底向上删除）；v65 隐藏项过滤与显示开关（递归下载沿用视图）；单文件 16 GiB | 目录任务、ReadAllFiles/ReadEmptyDirs、盘符 | **已实现**分页、传输主流程、递归删除和隐藏项开关。多共享根/盘符、压缩块和万条目录稳定性仍属 G18；递归删除见 [文件夹递归删除](recursive-remove-2026-11-10.md)，隐藏项见 [隐藏文件浏览选项](hidden-files-2026-11-10.md)。[P18][R14] |
| 终端、摄像头、端口转发 | ConPTY、持久终端/回放；CameraRelay 共享采集编码；TCP 转发目标限制/配额 | terminal/camera video/TCP tunnel 服务 | **已实现/待验**。SYSTEM/用户上下文、设备占用、撤权、断线清理和重连回放需逐项实机验收。[P19][R05][R15] |
| 存活、回退与重连 | UDP/TCP 直连失效检测、auto 回退、中继/carrier、新会话重查；QUIC 数据路径保活 | 连接流存活检查、结束清理和新会话建立 | **已实现**。真实公网 NAT/IPv6/代理/弱网切换仍需按网络记录；本机回环不能替代。[P20][R16] |
| 会话期间防休眠 | 默认开启 `keep_awake`；桌面连接请求系统+显示器，文件/终端只请求系统；按连接独立释放，待审批不持有 | 入站连接 WakeLock，Windows `SetThreadExecutionState` | **已实现**。真实电源策略、无人观看/传文件长时间会话和退出恢复待验。[P21][R17][R21] |
| 主机录屏与审计 | `record_incoming` 默认关闭；共享源压缩数据异步写 MP4，同源多人只录一份；H.264/HEVC/AV1/VP9、重建/定时分段、积压关键帧恢复 | video service 的 `record_incoming` recorder | **已实现**。只录视频；主机侧磁盘容量/保留、失败提示、服务账户和物理摄像头待验收。查看端出站自动录制已由 G11 补齐，见 [查看端自动录制](viewer-auto-record-2026-11-10.md)。[P22][R18] |
| 远程打印、角色互换、平台 | 未找到打印或 Switch Sides 协议；主机仅 Windows | printer service、SwitchSides、多平台被控 | **缺失/产品项**。涉及双端和部署，按 G19 单独立项；不与 Windows 主机稳定性修复混在一起。[P23][R19][R22] |

## 2. 当前剩余差距（发射端）

以下按开发顺序排列，编号与 [当前状态与计划](status-and-plan-2026-11-09.md) 及 [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md) 的 G 编号一致。前六项是代码/产品缺口，最后一项是环境验收，不作为代码缺陷计入。键盘 Legacy（G10）已于 v63 实现，查看端出站自动录制（G11）已于 2026-11-10 实现，均不再列为剩余缺口。

1. **G02 连接 ACL（最高优先）**：账号/组 → 设备策略，禁用设备或账号、移组、过期票据均能拒绝；直连、打洞、relay/carrier 都受同一守门，不允许退回旧 link key 绕过。账号 schema 已升到 v10，服务端设备策略存储/管理 API/签名回执有自动化覆盖；管理台新增仅 admin 的“设备策略”页，只有签名回执版本等于当前策略且 applied=true 才显示“已应用版本 N”，否则显示“待应用/应用失败”；lookup 也已区分账号与共享凭据并拒绝 restricted 设备上的共享凭据；服务端新增账号令牌换 60 秒票据端点，绑定设备指纹、账号、客户端公钥和策略上限，并有端点测试；每次注册响应已携带 `device_policy`（读取失败以 503 拒绝续租）。Go 主机代理已解析策略并经 `HostInfo.Policy`/`chast-tunnel` 事件上报，且原子写入与主机共享的 `<stem>.policy.json`；C++ 每个新会话读取该文件，受管或损坏时失败关闭（`Declined`，覆盖直连/打洞/relay/carrier 共用入口）；主机在共享标记匹配后写 `<stem>.policy-ack.json`，agent 用设备密钥签名异步上报应用回执（版本/模式/更新时间精确匹配，失败下次注册重试）；C++ 票据校验库也已实现并有冻结黄金向量与错误输入测试（`tests/access_ticket_test.cc`），并已接入 `HostSecurity`：`host.ini` 的 `access_issuer/access_kid/access_public_key` 与 `tunnel_id` 构成固定项，受管设备只接受验签通过、版本等于共享标记且未重放的票据，权限取票据 ∩ `permission_ceiling` ∩ 本机默认 ∩ 查看器请求，`chast-host-security-test` 覆盖黄金票据及全部否定路径；但票据尚未进入 v62 Hello、客户端不会携带，lookup 不返回票据，存量会话撤销未实现，客户端从直连地址仍可绕过 lookup 先取票这一环，因此连接 ACL 仍未端到端生效。设计约束见 [受管准入与会话授权设计](access-control-design-2026-11-09.md)。验收：新 lookup、先取票再撤权、已连接后撤权三种状态 × 各连接路径。
2. **G03 账号级会话权限票据**：在 G02 的独立签发/租约框架上，权限取票据上限、设备策略、主机本地策略和用户同意的交集，覆盖全部敏感入口；撤权停止终端/转发/文件等正在执行的操作，存量会话最多 65 秒失效。
3. **AV1 4:4:4**：v61 已提供 VP9 profile 1（I444）；RustDesk 的 `use_i444` 仅作用于 VP9/AV1，因此 AV1 4:4:4 仍是色彩路径的剩余差距。H.264/HEVC 两边都不提供 I444。
4. **远程打印与 Switch Sides**：协议和两端入口都没有，属于产品项（G19）；需要双端协议与部署支持，不与主机稳定性修复混做。
5. **文件管理剩余项（G18）**：v64 已提供文件夹递归删除和确认，v65 已提供隐藏项过滤和显示开关（列表与递归下载同一视图）；多共享根/盘符、压缩块传输仍未提供。万条目录与递归下载稳定性已由 v56 分页覆盖，但真实素材和跨网吞吐仍待复验。共享根和容量上限是安全边界，不为了对齐直接取消。
6. **虚拟显示驱动与 HDR 端到端**：PLAT-01 的代码路径存在，但驱动分发被第三方条款与固定哈希阻塞；HDR 目前采集归一为 SDR，不能宣称 HDR10 端到端。前者是外部依赖，后者是明确的产品边界。
7. **环境验收（不是代码缺口）**：真实多物理屏/混合 DPI/多 GPU、真实 UAC 与跨管理员账户、多用户 RDP、物理摄像头、真实 HDR、Explorer/Office 粘贴、其他键盘布局/IME、跨公网 NAT/IPv6/代理/中继、弱网和不同显卡故障恢复。需要在对应设备或网络执行并记录证据边界。

## 3. 已修复项与验证边界

旧报告的 F01–F08 所描述的结构性差距在 v50–v61 已逐项处理，不能再按 11 月 7 日的快照列为现存缺陷：

- F01 静止首帧/关键帧：`NoNewFrame` 使用有效 GPU 缓存帧，按编码器/后端/画质预算刷新，IDR 入队清空旧刷新帧。
- F02 跟随会话请求：显示跟随、OS 登录、WTS 列表/选择都进入 handler 并返回有界结果；独立选屏已由按屏订阅取代。
- F03 全局策略传播：共享策略掩码、版本同步和传输层限制；全局关闭传播到已注册及待审批连接，重新启用不恢复旧授权。
- F04 音频权限独立：注册表级 `SharedLoopback`，按连接订阅/权限/发送，发布者静音或拥塞不影响其他有权限者。
- F05 独立选屏：`DesktopStreamPool` 按显示源共享采集/编码，每连接独立订阅和发送；发布者离开不重建他人仍在使用的流。
- F06 全订阅者 QoS：全局 `VideoQos` 汇总所有订阅者，每连接独立发送队列、积压丢弃和关键帧恢复。
- F07 按 ID 管理：稳定连接 ID，聊天/文件/权限/断开都有显式目标接口，不再依赖 `focused` 会话。
- F08 编解码能力协商：Hello 携带 `decoding_codecs`/`decoding_444`，共享 codec 组取订阅连接交集，Auto 回退 OpenH264；v61 支持 VP9 4:4:4。

这些行为分别有 `tests/desktop_stream_test.cc`、`tests/host_parity_test.cc`、`tests/host_multi_test.cc`、`tests/host_voice_test.cc`、`tests/clipboard_async_test.cc`、`tests/mp4_recorder_test.cc` 等回归。本轮做文档比对时只核对了源码和测试断言，没有重新运行完整产品测试，也没有触发本机输入、登录、锁屏或修改显示拓扑；旧测试成绩不能被改写为本轮双机 A/B 结果。

v56–v63 追加能力（目录分页、剪贴板方向、空目录、按需文件剪贴板、远程提权、VP9 4:4:4、事件驱动剪贴板、主机录屏、防休眠、服务审批、v62 票据携带/续租与运行中撤权、v63 键盘 Legacy）均已在模块表标注其验证边界。源码可以确认入口和协议字段存在，实际首屏、弱网延迟、色彩、听感、硬件恢复和驱动兼容性仍需真实设备对照。

## 4. 源码证据

本项目路径相对仓库根目录，行号为本次核对 `ddf75d2` 加工作区 R01–R05 修复时的位置；后续修改后应按函数名/类名定位。RustDesk 路径以 `rustdesk@7423dce:` 前缀表示固定提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d` 中的文件，可通过本地 `D:/rustdesk` 的 `git show`/`git grep` 复核，不再使用旧的 `D:/rustdesk-ref` 路径。

[P01]: host/service.cc:259（服务启动桌面会话 worker）、host/worker.cc:368（服务审批）
[P02]: src/host/host_security.cc:168、`:482`（审批决策）
[P03]: host/elevation_handoff.cc、host/main.cc:781、src/net/quic.cc:3564
[P04]: src/host/host_server.cc:1436、`:1474`（按连接 ID 改权限/聊天）
[P05]: src/host/host_server.cc:8254（活跃 worker 消费共享策略版本）、`:142`
[P06]: src/host/desktop_stream.h:18、src/host/host_server.cc:2388
[P07]: include/chast/protocol.h:1496、src/host/desktop_stream.cc:1105、`:1136`
[P08]: include/chast/protocol.h:1041、src/capture/nv12_pass.cc、src/codec/vp9_encoder.cc
[P09]: src/host/desktop_stream.h:99（静止刷新预算）
[P10]: src/capture/dxgi_capturer.cc:253、src/capture/detail.h:285
[P11]: src/host/video_qos.h:14、src/host/host_server.cc:3660
[P12]: src/host/host_server.cc:2689（注册表级共享系统音源）
[P13]: include/chast/input.h:5、src/win/input.cc
[P14]: src/host/host_server.cc:331、`:390`、`:8488`（会话选择/OS 登录）
[P15]: host/worker.cc:271（虚拟显示探测）、src/host/display_privacy.cc
[P16]: src/clipboard/clipboard_listener.cc:48、`:86`；include/chast/protocol.h:1265（方向）
[P17]: src/clipboard/file_transfer.cc、include/chast/protocol.h:435（`ClipboardFileRead`）
[P18]: include/chast/protocol.h:267、`:775`（v56 分页游标）
[P19]: src/host/host_server.cc:4948（端口转发）、`:4950`（终端）、`:4956`（摄像头）
[P20]: src/net/quic.cc、src/net/carrier.cc、docs/rustdesk-alignment-2026-11-08.md
[P21]: include/chast/host_settings.h:191、src/win/power_request.cc:35
[P22]: include/chast/host_settings.h:108、src/host/stream_recorder.h:20、src/host/host_server.cc:2402
[P23]: 全仓检索 `printer`、`SwitchSides` 无入口；平台范围见 CMakeLists.txt
[R01]: rustdesk@7423dce:src/server.rs
[R02]: rustdesk@7423dce:src/server/connection.rs（批准、WTS、OS 登录）
[R03]: rustdesk@7423dce:src/server/connection.rs（`ElevationRequest`/portable service）
[R04]: rustdesk@7423dce:src/ui_cm_interface.rs、flutter/lib/desktop/pages/server_page.dart
[R05]: rustdesk@7423dce:src/server.rs、src/server/connection.rs（显示器/音频订阅）
[R06]: rustdesk@7423dce:src/server/connection.rs（`supported_decoding` 协商）
[R07]: rustdesk@7423dce:src/server/video_service.rs、video_service/static_refresh.rs
[R08]: rustdesk@7423dce:src/server/video_qos.rs
[R09]: rustdesk@7423dce:src/server/audio_service.rs
[R10]: rustdesk@7423dce:src/server/input_service.rs（Map/Translate/Legacy）
[R11]: rustdesk@7423dce:src/server/video_service.rs（隐私/虚拟显示分支）
[R12]: rustdesk@7423dce:src/server/connection.rs、src/clipboard.rs
[R13]: rustdesk@7423dce:libs/clipboard/src/platform/windows.rs、libs/clipboard/src/windows/wf_cliprdr.c
[R14]: rustdesk@7423dce:src/server/connection.rs、libs/base/src/fs.rs
[R15]: rustdesk@7423dce:src/server/terminal_service.rs
[R16]: rustdesk@7423dce:src/client/io_loop.rs
[R17]: rustdesk@7423dce:src/server/connection.rs（入站 WakeLock）
[R18]: rustdesk@7423dce:src/server/video_service.rs（`record_incoming`）
[R19]: rustdesk@7423dce:src/server/printer_service.rs、src/server/connection.rs（`SwitchSides`）
[R20]: rustdesk@7423dce:libs/scrap/src/common/codec.rs（`use_i444` 仅 VP9/AV1）
[R21]: rustdesk@7423dce:src/platform/windows.rs（`SetThreadExecutionState` WakeLock）
[R22]: rustdesk@7423dce:src/server.rs、各平台被控实现

补充定位：发送容量与等待 `src/net/quic.cc`；全订阅者 QoS 决策 `src/host/video_qos.h`、`src/host/desktop_stream.cc`；文件大小/目录数量上限 `include/chast/protocol.h`；协议版本 `include/chast/protocol.h:57`；当前回归入口 `tests/desktop_stream_test.cc`、`tests/host_parity_test.cc`、`tests/host_multi_test.cc`、`tests/host_voice_test.cc`、`tests/clipboard_async_test.cc`、`tests/mp4_recorder_test.cc`。G02/G03 的签发、策略同步和租约约束见 `docs/access-control-design-2026-11-09.md`；其中设备策略存储/管理 API、lookup 准入、服务端换票端点和管理台设备策略回执页已实现并有自动化，注册响应已携带 `device_policy`，Go 代理已解析并经 `HostInfo.Policy`/`chast-tunnel` 事件上报并落盘到共享 `<stem>.policy.json`，C++ 对受管设备的新会话失败关闭，并已形成签名回执闭环（主机确认文件 + agent 设备密钥异步上报）；C++ 票据校验库（`src/host/access_ticket.cc`）已通过黄金向量测试并接入 `HostSecurity` 准入（固定项来自 `host.ini`，权限交集与拒绝路径有 `host_security_test` 覆盖）；v62 已让客户端在 Hello 携带票据、lookup 返回票据并支持 20 秒续租与运行中撤权；撤销推送和真实链路验收仍待完成。
