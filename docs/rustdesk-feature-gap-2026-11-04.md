# 与本地 RustDesk 的功能差距核查（2026-11-04）

> 当前实现快照、验证证据和后续步骤见 [Windows 开发交接](windows-development-handoff-2026-11-05.md)。

后续实现增量：[远端显示管理与隐私选择](remote-display-control-2026-11-05.md)。当前协议为 42，已支持临时虚拟输出数量调整、稳定 ID 指定输出停用/重启用、原布局恢复和隐私实现选择；多人共同管理等边界仍待补齐。

2026-11-05 复核补充：[Windows 功能审核](windows-functional-audit-2026-11-05.md)。当前确认 13 项功能工作流尚缺或不完整；会话结束锁屏已接入协议 40、主机和查看器，策略测试通过，转入待实机验收。远端虚拟屏管理、独立辅助连接、屏幕跟随及相对鼠标模式仍缺完整流程；虚拟屏本机实测状态以新报告和验证记录为准。

本次按源码、协议和界面入口核查。结论是：基础远程控制、文件、音视频和自建服务已有较广覆盖；主要差距集中在无人登录时的接入、多会话，以及高级协助功能。不能用旧任务表中的未勾选项直接代表当前缺口。

## 比对基线与边界

| 项目 | 实际目录 | 基线 |
| --- | --- | --- |
| ds-chcast | `D:\ds-chcast` | `63f7ba2`，2026-11-04，包含当前工作区尚未提交的修复 |
| RustDesk 客户端 | `D:\proj\rustdesk` | `4b92080`，2026-10-20，Cargo 版本 1.5.1 |
| RustDesk 开源服务端 | `D:\proj\rustdesk-server` | `873230e`，2026-10-10，Cargo 版本 1.1.17 |

`D:\rustdesk` 不存在；`D:\rustdesk-src` 没有可用源码检出。旧文档里的目录、提交和部分功能状态不再适合作为本次依据。

这是本地版本的静态功能比较，未编译或运行 RustDesk，也未做双机、跨网、驱动安装及性能对测。下文“已有”表示存在实现与调用入口，不等于已完成生产环境验收。没有将 RustDesk Pro 服务端能力算作开源基线，也没有核对线上最新版本。

优先级是建议的产品开发顺序：P0 表示会阻断目标核心场景，P1 表示主要兼容性或工作流差距，P2 表示按需求安排；不是安全漏洞评级。

## 已确认的实施约束（2026-11-04 用户确认）

- 视频采集、处理、编码、解码、呈现保持 GPU 零拷贝；不加入 GDI/CPU 采集、软件编解码 fallback。
- 硬件能力不足时明确报告不支持；不同硬件编码器之间的 GPU 路径选择可以保留。
- G02 的 CPU 兼容性兜底不纳入任务，也不再作为产品缺口。更多编码/色度格式仅在可实现 GPU 零拷贝的条件下推进。
- 先完成 Windows 功能对齐；Linux/macOS/移动端随后推进。
- 对齐可用功能与工作流，不改成 RustDesk 线协议；Pro 服务端与未提供的 Web 核心不自动纳入本地基线。

## 已确认的功能差距

| 编号 / 建议优先级 | RustDesk 本地源码能力 | 我们的现状与缺口 | 代码依据 |
| --- | --- | --- | --- |
| G01 / P0（无人值守目标） | Windows 服务启动特权远程进程，并有登录前状态处理 | **代码已修复，实机待验收**：服务按活动控制台启动 worker，不再要求登录用户名；注销事件结束旧 worker 后重新启动，清理旧连接和终端。重启无人登录、注销、锁屏/UAC 仍需专机验收 | 我们：[service.cc:180](D:/ds-chcast/host/service.cc:180)、[service.cc:385](D:/ds-chcast/host/service.cc:385)；对方：[windows.rs:693](D:/proj/rustdesk/src/platform/windows.rs:693)、[windows.rs:813](D:/proj/rustdesk/src/platform/windows.rs:813)、[windows.rs:1254](D:/proj/rustdesk/src/platform/windows.rs:1254) |
| G02 / 不纳入 | 对方有 GDI 和软件编解码路径 | **明确的产品约束**：保留 DXGI 与 GPU 硬编/硬解，不加入 CPU fallback；能力不足明确报错 | 用户于 2026-11-04 确认 |
| G03 / P1 | 被控端维护连接集合；控制端有多个远程会话标签 | **核心实现完成，实机待验收（2026-11-05）**：主机/服务 worker 支持 8 个独立认证会话，查看器支持多个窗口，逐会话权限、聊天、文件与退出隔离；直连、UDP/TCP/WS 中继均已复用改造。真实双机/跨网、多窗口运行仍需验收 | 我们：`src/net/quic.cc`、`src/host/host_server.cc`、`app/windows/runner/main.cpp`、`server/internal/relay/multiplex*.go`；对方：`D:\proj\rustdesk\src\server.rs`、`flutter/lib/desktop/pages/remote_tab_page.dart` |
| G04 / P1 | 可选择订阅多个显示器的视频 | **独立窗口已实现，部分边界待补（2026-11-05）**：显示器菜单可在新窗口建立独立 GPU 视频连接；同屏多会话与合并捕获共享 DXGI 来源，各读者独立缩放。新窗口仍独立认证，一次性口令用尽后需要密码/信任/批准；多适配器协商与硬件验收仍缺 | 我们：`app/lib/pages/session_page.dart`、`app/lib/session_window.dart`、`src/capture/shared_capturer.cc`；对方：`D:\proj\rustdesk\src\server\connection.rs` |
| G05 / P1 | 有禁用本地输入控制，以及虚拟显示隐私模式中停用物理屏幕的实现 | **部分实现**：我们的隐私模式是 Default 桌面的置顶黑色透明鼠标遮罩，并从采集中排除；已增加独立本地键鼠禁用权限/请求/状态与释放路径；已补齐可选虚拟屏隐私路径：原拓扑快照、GPU 预检、物理屏停用、会话恢复、独立恢复进程和托盘启停；已在 Intel UHD 730 完成 usbmmidd 安装、真实 GPU 切换/撤权/断线/强杀主机及服务同配置 Job 恢复验收；安全桌面、SCM 服务全链路及其他 GPU 矩阵仍待验收。详见 `docs/virtual-display-privacy-2026-11-05.md` | 我们：[privacy.cc:137](D:/ds-chcast/src/win/privacy.cc:137)、[privacy.cc:159](D:/ds-chcast/src/win/privacy.cc:159)；对方：[connection.rs:1287](D:/proj/rustdesk/src/server/connection.rs:1287)、[win_virtual_display.rs:459](D:/proj/rustdesk/src/privacy_mode/win_virtual_display.rs:459)。对方同样受系统、权限和驱动条件限制 |
| G06 / P1–P2 | 有远程提权请求、Windows 会话选择和控制双方互换流程 | **缺失这些工作流**：现有服务提权与安全桌面支持不能代替远端发起提权；当前 worker 跟随活动控制台，没有 RDP/多用户会话选择。未找到会话内交换控制双方的协议与界面入口 | 我们：[service.cc:385](D:/ds-chcast/host/service.cc:385)、[protocol.h](D:/ds-chcast/include/chast/protocol.h)；对方：[connection.rs:2276](D:/proj/rustdesk/src/server/connection.rs:2276)、[connection.rs:3957](D:/proj/rustdesk/src/server/connection.rs:3957)、[connection.rs:3982](D:/proj/rustdesk/src/server/connection.rs:3982)；会话选择在对方也有安装模式、share RDP 等条件 |
| G07 / 战略项 | 原生 Windows/Linux/macOS 与 Android/iOS 代码及打包入口，平台能力各有差别 | **客户端平台缺失**：我们 C++ 主链路要求 MSVC/Windows，尚无可用的非 Windows 远程桌面客户端/被控端产品链路。Go 服务端可移植不代表桌面端跨平台；iOS 也不能按 RustDesk 双向被控能力计算 | 我们：[CMakeLists.txt:13](D:/ds-chcast/CMakeLists.txt:13)；对方：[lib.rs:1](D:/proj/rustdesk/src/lib.rs:1)、[pubspec.yaml:135](D:/proj/rustdesk/flutter/pubspec.yaml:135) |
| G08 / P2 | 有远程打印服务和配套库，运行时还依赖打印驱动适配 DLL | **缺失**：未找到打印重定向的协议、队列、驱动接入和 UI。文件传输后手动打印不能算同一功能 | 对方：[printer_service.rs:9](D:/proj/rustdesk/src/server/printer_service.rs:9)、[printer_service.rs:88](D:/proj/rustdesk/src/server/printer_service.rs:88)；我们旧任务：[PLAT-02](D:/ds-chcast/docs/rustdesk-gap-tasks-2026-11-01.md:46)。此项不能仅凭对方源码就认定其驱动已在本机可用 |
| G09 / P2 | 多终端会话及可选断线保留的终端服务 | **部分实现**：已接入两个独立 ConPTY 终端标签、独立输入/输出/尺寸和单独关闭，并通过真实 QUIC/ConPTY 验证。断线或权限撤销仍结束控制台；尚缺显式启用的断线保留/恢复，不能通过取消清理直接实现 | 我们：[terminal_tabs.dart](D:/ds-chcast/app/lib/widgets/terminal_tabs.dart)、[terminal_relay.h](D:/ds-chcast/include/chast/terminal_relay.h)；对方：[terminal_service.rs:890](D:/proj/rustdesk/src/server/terminal_service.rs:890) |
| G10 / P2 | VP8/VP9/AV1 以及 i444 编码协商路径 | **编码种类与色度选择较少**：我们仅 H.264/HEVC，当前硬编输入为 NV12。可进一步补文字画质选项与更多编码器；收益需要实测，不能由编码名称推出画质或延迟一定优于现状 | 我们：[encoder.h:42](D:/ds-chcast/include/chast/encoder.h:42)、[ffmpeg_encoder.cc:388](D:/ds-chcast/src/codec/ffmpeg_encoder.cc:388)；对方：[codec.rs:134](D:/proj/rustdesk/libs/scrap/src/common/codec.rs:134)、[codec.rs:294](D:/proj/rustdesk/libs/scrap/src/common/codec.rs:294) |
| G11 / P2 | 有触摸缩放/平移事件，以及在被控端显示协助者光标的能力 | **手势已实现，标识仍缺（2026-11-05）**：Windows 触屏/触控板接入平移和捏合，缩放使用 Ctrl+滚轮语义，具有独立协议事件、权限校验和按键恢复；待触控硬件实测。协助者标识光标尚未实现。对方 Windows 当前消费 ScaleUpdate，Pan 事件的主要处理在移动端；不能仅凭协议类型认定所有平台行为相同 | 我们：`app/lib/widgets/remote_view.dart`、`include/chast/protocol.h`、`src/win/input.cc`；对方：`D:\proj\rustdesk\src\server\input_service.rs:1233`、`:1519`、`src/whiteboard/mod.rs` |
| G12 / P2 | 多语言资源及本地化界面 | **覆盖较少**：我们仅简体中文与英文，其他语言尚无产品级覆盖 | 我们：[main.dart:35](D:/ds-chcast/app/lib/main.dart:35)；对方：[lang](D:/proj/rustdesk/src/lang) |

G03 的传输/主机/中继/权限/输入隔离改造已于 2026-11-05 接入并通过本地并发回归。GPU 视频链路继续保持无 CPU 像素回退；该结果不替代双机、跨网和多显卡验收。

## 已有实现，不能继续列为完全缺失

| 能力 | 当前代码证据与边界 |
| --- | --- |
| NVIDIA / AMD / Intel 硬编 | [ffmpeg_encoder.cc:72](D:/ds-chcast/src/codec/ffmpeg_encoder.cc:72) 已有 NVENC、AMF、QSV；另有 oneVPL。后续补更多硬件格式及各厂商实机覆盖；软件兜底明确排除 |
| 文件、剪贴板、音频、麦克风、终端、摄像头、聊天、端口转发、录制、重启 | [protocol.h](D:/ds-chcast/include/chast/protocol.h)、[remote_toolbar.dart](D:/ds-chcast/app/lib/widgets/remote_toolbar.dart)、[file_browser_panel.dart](D:/ds-chcast/app/lib/widgets/file_browser_panel.dart) 已有协议/界面入口；这不代表文件系统边界、终端保活等细节全部与 RustDesk 相同 |
| NAT 协调、中继、受限网络回退 | [rendezvous.go](D:/ds-chcast/server/internal/rendezvous/rendezvous.go)、[carrier/frame.go:1](D:/ds-chcast/server/internal/carrier/frame.go:1)、[relaypool/pool.go](D:/ds-chcast/server/internal/relaypool/pool.go) 已有 UDP 打洞、QUIC 数据面、TCP/WS 载体和中继池；另有 IPv6、SOCKS5、LAN、UPnP、WOL 模块。真实成功率仍需跨网验收 |
| 身份与会话权限 | [host_security.cc](D:/ds-chcast/src/host/host_security.cc)、[protocol.h:811](D:/ds-chcast/include/chast/protocol.h:811) 已有主机认证与会话权限。账户、TOTP、WebAuthn、设备组等也有服务端实现，不能直接套用“尚无账号系统”的旧结论 |
| 虚拟显示 | [virtual_display.cc](D:/ds-chcast/src/host/virtual_display.cc)、[virtual_display.h:18](D:/ds-chcast/include/chast/virtual_display.h:18) 已有管理代码、策略和本地接线；本机驱动/GPU/隐私恢复实测已通过，仍缺正式交付和其他环境矩阵，详见新审核 |
| 更新签名与回滚 | [manifest.go:75](D:/ds-chcast/server/internal/software/manifest.go:75) 有发布者签名验证；[apply_update.go:95](D:/ds-chcast/server/cmd/chast-tunnel/apply_update.go:95) 有查看器安装备份和健康检查回滚。主机安装器启动路径的保障范围不同，不能笼统写“全部没有”或“全部完成” |
| 元数据签名、追踪与审计 | 当前代码已包含身份签名、[tracing](D:/ds-chcast/server/internal/tracing/tracing.go)、分组审计与归档模块。旧状态页中相关“未实现”描述需要按具体章节日期核对 |

## 实现存在，但交付或验收仍有缺口

1. **虚拟显示驱动交付**：2026-11-05 已固定官方 ZIP 与逐文件哈希，并完成本机安装和 GPU 虚拟屏验收；再分发许可材料和专用无头测试机仍未完成。属于“部分完成/交付待验证”，不是“零实现”。另有 CLI 卸载不感知活动会话的已知边界。见 [fetch-usbmmidd.ps1:26](D:/ds-chcast/tools/fetch-usbmmidd.ps1:26) 和 [验收手册:489](D:/ds-chcast/docs/plat-01-acceptance-runbook-2026-11-04.md:489)。
2. **网络实测**：双机跨 NAT、真实 IPv6、对称 NAT、UPnP、TCP/WS 受限网络回退需要补可复现记录。不能从有实现和单元测试推出各种网络下均能连接。
3. **媒体与桌面实测**：不同厂商 GPU、HEVC、双机音频、AEC、摄像头、端点热插拔、多显示器负坐标/高 DPI、锁屏/UAC、无头重启需要补矩阵。原有 [ENV 任务:22](D:/ds-chcast/docs/rustdesk-gap-tasks-2026-11-01.md:22) 仍可用作验收索引，但不能把其中旧开发状态全盘照搬。

这些是我们尚缺的证据，不表示已经实测 RustDesk 在对应环境全部通过。

## 服务端差异与避免误判的项目

- **PostgreSQL 不是本地开源 RustDesk 的对齐缺口。** 对方 [database.rs:3](D:/proj/rustdesk-server/src/database.rs:3) 使用 SQLite，Postgres/MySQL 导入只是注释。若为我们自身的多实例或运维目标增加 PostgreSQL，应另行论证。
- **传输栈不同，不能直接等同。** 对方本地原生客户端及 hbbs 有 TCP 打洞路径，见 [client.rs:930](D:/proj/rustdesk/src/client.rs:930)、[rendezvous_server.rs:103](D:/proj/rustdesk-server/src/rendezvous_server.rs:103)；我们主要是 UDP 打洞、QUIC 与 TCP/WS 封装回退。没有对应的 TCP 点对点打洞策略是策略覆盖差异，是否影响某类网络的成功率需要实测。不能简单沿用旧文档“RustDesk 原生数据面就是 WebRTC/ICE/TURN”的概括。
- **RustDesk 协议互通尚未实现。** 我们使用自己的协议、身份和信令格式；能够替代类似使用场景不代表可以直接连接 RustDesk 客户端或使用 hbbs/hbbr。若无互通需求，这属于架构选择。
- **浏览器远控不能按这份源码判定已完整具备。** RustDesk 本地树有 Flutter Web bridge，但其 [AGENTS.md](D:/proj/rustdesk/AGENTS.md) 说明核心 TS/JS 客户端目录不在该检出中。因此浏览器远控可作为我们的产品方向，不能据此给出已验证的逐项差距。
- **企业账号能力不与 Pro 混算。** 我们已经有分组管理/审计 API；[accessgroups.go:38](D:/ds-chcast/server/internal/api/accessgroups.go:38) 的成员角色检查用于分组接口，而 [api.go:609](D:/ds-chcast/server/internal/api/api.go:609) 的主机查询没有按设备组检查成员关系。这说明“组管理权限”还不能自动等同于“组级远控准入”；主机自身认证仍独立存在。若要企业集中授权，应补用户身份到连接授权的链路，不能称为当前开源 RustDesk 服务端已经验证领先的功能。
- **主机更新保障需要单独补齐。** [host.go:585](D:/ds-chcast/server/internal/agent/host.go:585) 校验、下载后启动安装器；该路径没有查看器更新器的健康检查/回滚协调。可作为自身可靠性任务，不宣称本地 RustDesk 已具备同等或更强回滚。

## 建议推进顺序与验收目标

| 顺序 | 工作包 | 可验证的完成标准 |
| --- | --- | --- |
| 1 | 无人登录可接入 + 驱动交付前置 + 跨网验收 | 未登录重启、注销、已登录锁屏和 UAC 分别能连接/恢复；真实无头启动有记录；直连、中继、TCP/WS 回退有双机证据 |
| 2 | 会话架构 + 隐私控制 | 查看器并发连接两个目标、被控端同时接收两个授权连接；本地输入阻断/恢复及物理屏隐私效果可观察验证；视频全程保留 GPU 路径 |
| 3 | 多屏独立显示、提权/会话选择、终端保留、打印 | 各功能有明确权限边界和恢复行为；两个显示器可独立查看；断线终端仅在显式授权时保留；打印端到端走通 |
| 后续 | 跨平台、更多硬件编码、多语言 | Windows 对齐后推进其他平台；新增视频格式必须保持 GPU 零拷贝；浏览器核心不在当前本地基线中 |

本报告创建时为静态比较；后续实施以 [Windows 对齐进度](D:/ds-chcast/docs/windows-parity-progress-2026-11-04.md) 为准，未完成项不得标为已对齐。
