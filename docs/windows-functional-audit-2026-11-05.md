# Windows 功能缺口复核（2026-11-05）

> 快照更新：本文对应的本批代码已提交到 `feat/windows-session-display-20261105` 的 `f5bc719`。后续开发请先阅读 [Windows 开发交接](windows-development-handoff-2026-11-05.md)；下文的“未提交工作区”描述仅指初次审核当时的状态。

本次审核当前未提交工作区、实际协议/界面/服务处理代码，以及现有验证日志。对照本地 `D:/proj/rustdesk`（`4b92080`、1.5.1）和 `D:/proj/rustdesk-server`（`873230e`、1.1.17）。不对照线上最新版本，不将 RustDesk Pro 纳入开源基线；未运行 RustDesk 做双机性能对测。初次审核仅更新文档；后续实现及本机显示验证增量见下文及 `remote-display-control-2026-11-05.md`。

结论：Windows 仍未完成全部功能对齐。当前清单有 **13 项功能工作流尚缺或不完整**，另有严格零拷贝、实机验收及交付问题；这是本轮已确认项，不代表穷尽所有行为差异。多人连接、双终端、独立屏幕窗口、虚拟屏隐私已经有实现，其中虚拟屏完成本机实测。**W07 会话结束锁屏已接入协议、主机及查看器，从功能缺失移到待验收项**；当前协议为 42；W06 的远程数量管理、指定输出停用/重启用及隐私实现选择已接入，仍保留多查看者组合等边界。

## 用户约束与零拷贝状态

- 先 Windows，再跨平台；CPU/GDI 采集和软件编解码 fallback 不纳入补齐任务。
- 当前无 CPU 像素回退，但**严格的纹理零拷贝尚未达成**：共享采集有 GPU `CopyResource`，FFmpeg 编码有 GPU `CopySubresourceRegion`，oneVPL 可接受 `IMPORT_COPY`。实机日志为 `shared_import=0`。不能把“像素不经过 CPU”写成“完全没有 GPU 拷贝”。
- 证据：[共享采集](D:/ds-chcast/src/capture/shared_capturer.cc:96)、[FFmpeg 编码](D:/ds-chcast/src/codec/ffmpeg_encoder.cc:623)、[oneVPL 导入](D:/ds-chcast/src/codec/vpl_encoder.cc:854)、[实机日志](D:/ds-chcast/build/align-vdisplay-live-full.log:8)。这是用户约束符合性问题，不宣称 RustDesk 实现了全链路无 GPU 拷贝。

## 尚缺或不完整的 Windows 工作流

优先级表示建议实施顺序，不是安全漏洞评级。负向结论基于当前协议、UI、实现调用链和相关检索，不把关键词未出现单独当成证据。

| 编号 / 优先级 | 缺口及用户影响 | 我们的现状 / 证据 | 本地 RustDesk 对照 |
| --- | --- | --- | --- |
| W01 / P1 | **远端请求提权**：便携主机不能在会话中由查看器发起完整提权及重连流程 | [协议](D:/ds-chcast/include/chast/protocol.h:70) 无相应请求/结果；[本地提权](D:/ds-chcast/host/platform.cc:177) 用于本机 UAC，不等同远端请求 | [ElevationRequest 处理](D:/proj/rustdesk/src/server/connection.rs:3957) |
| W02 / P1 | **Windows/RDP 会话列表与选择**：无法指定连接到另一个系统用户会话 | [服务](D:/ds-chcast/host/service.cc:387) 仅跟随活动控制台。多查看器连接同一桌面不等同多系统会话 | [SelectedSid 处理](D:/proj/rustdesk/src/server/connection.rs:4021)、[WindowsSessions](D:/proj/rustdesk/libs/base/protos/message.proto:815)，对方也有安装/权限条件 |
| W03 / P2 | **控制双方互换及切回** | 当前协议/工具栏无完整双方角色互换、授权、重连入口 | [SwitchSidesRequest](D:/proj/rustdesk/src/server/connection.rs:3982) |
| W04 / P1 | **独立终端/端口转发/摄像头连接**：这些能力目前依赖桌面会话，桌面 GPU 建链失败时不能单独使用 | [SessionMode](D:/ds-chcast/include/chast/protocol.h:1077) 只有 Desktop/FileTransfer；[主机分流](D:/ds-chcast/src/host/host_server.cc:1218)，三个 relay 在桌面流内创建于 [3690 行](D:/ds-chcast/src/host/host_server.cc:3690)。纯文件连接已经存在 | [LoginRequest](D:/proj/rustdesk/libs/base/protos/message.proto:68) 有独立 PortForward/ViewCamera/Terminal 分支；[终端入口](D:/proj/rustdesk/flutter/lib/desktop/pages/connection_page.dart:570) |
| W05 / P1 | **终端断线保留与恢复**：重连无法接回之前 shell | 双 ConPTY 已实现，但 [TerminalRelay 析构](D:/ds-chcast/src/host/terminal_relay.cc:115) 结束控制台；无持久 service ID、恢复授权及输出回放流程。不能靠简单取消清理完成 | [持久终端服务](D:/proj/rustdesk/src/server/terminal_service.rs:161)、[重连 service_id](D:/proj/rustdesk/libs/base/protos/message.proto:89) |
| W06 / P1 | **虚拟屏管理的多查看者及隐私组合流程仍不完整** | 数量控制、稳定 ID 指定输出停用/重启用、已有 chast 所有输出的临时管理、原布局恢复及隐私实现选择均已接入，真实 ViewerEngine/GPU/驱动及 5 项扩展恢复场景通过；仍缺多查看者共享管理、临时显示与隐私无缝切换及其验收。CCD 停用不等同按目标卸载 PnP 设备。见 [实现与验证](D:/ds-chcast/docs/remote-display-control-2026-11-05.md) | [ToggleVirtualDisplay/TogglePrivacyMode](D:/proj/rustdesk/libs/base/protos/message.proto:618)、[真实处理](D:/proj/rustdesk/src/server/connection.rs:4765) |
| W08 / P2 | **多屏新窗口的后续授权体验**：一次性口令首连用尽后，新屏窗口可能再次要求认证 | [SessionWindowRequest](D:/ds-chcast/app/lib/session_window.dart:11) 创建新连接并复用目标凭据；独立认证是现有安全行为，不能绕过。尚缺同一已授权会话内的派生授权或订阅工作流 | [CaptureDisplays](D:/proj/rustdesk/libs/base/protos/message.proto:612) 提供显示订阅操作 |
| W09 / P2 | **协助者光标标识**：本地用户不能区分哪个远端协助者在指示位置 | 当前 [CursorUpdate](D:/ds-chcast/include/chast/protocol.h:112) 为主机光标发往查看器；未接入远端协助者身份标识绘制 | [show_my_cursor/whiteboard](D:/proj/rustdesk/src/server/connection.rs:5108) |
| W10 / P2 | **跟随远端光标/前台窗口自动切屏** | 当前已有手动切屏、合并视图、独立窗口，但无该选项及跟随状态机 | [UI](D:/proj/rustdesk/flutter/lib/common/widgets/toolbar.dart:974)、[选项处理](D:/proj/rustdesk/src/server/connection.rs:4963)，对方也限制平台和显示模式 |
| W11 / P2 | **完整相对鼠标操作模式**：游戏/3D 应用所需的捕获、无限相对移动、退出/失焦恢复尚无完整 UI 流程 | [MoveRelative](D:/ds-chcast/include/chast/protocol.h:339)、[Windows 注入](D:/ds-chcast/src/win/input.cc:726) 已有；Flutter 仅有桥接常量，未找到查看器模式切换与持续输入链路，不能列为底层完全缺失 | [相对鼠标模式入口](D:/proj/rustdesk/flutter/lib/common/widgets/toolbar.dart:1365)、[输入处理](D:/proj/rustdesk/src/server/input_service.rs:1352) |
| W12 / P2 | **远程打印** | 缺打印任务协议、打印队列、驱动适配、查看器入口。已有文件下载不能算打印重定向 | [printer_service.rs](D:/proj/rustdesk/src/server/printer_service.rs:9)，对方依赖额外驱动组件，不代表本机已验证 |
| W13 / P2 | **更多视频编码/色度选择** | [VideoCodec](D:/ds-chcast/include/chast/encoder.h:42) 为 H.264/HEVC，当前硬编输入 NV12；缺 AV1 等及 4:4:4 文字画质选项。只在硬件端到端路径可用时补，不引入 CPU fallback | [SupportedDecoding](D:/proj/rustdesk/libs/base/protos/message.proto:654) 包含 VP8/VP9/AV1/i444；并非都在所有显卡上可硬编 |
| W14 / P2 | **更多产品语言** | [supportedLocales](D:/ds-chcast/app/lib/main.dart:39) 为简中/英文，其他语言缺完整界面资源 | `D:/proj/rustdesk/src/lang/` |

## 已实现，但仍欠验收或交付

**W07 会话结束自动锁屏：实现已接入，验收尚未闭环。** [协议](D:/ds-chcast/include/chast/protocol.h:320)、[主机退出策略](D:/ds-chcast/src/host/host_server.cc:1229)、[查看器工具栏](D:/ds-chcast/app/lib/widgets/remote_toolbar.dart:617) 均已接通。默认关闭，启用要求输入权限；多人时等最后一个桌面查看者退出，纯文件会话不阻挡，撤权取消意图。真实 QUIC/GPU 测试用替代回调验证触发、取消、撤权竞态、延迟执行及系统调用失败，没有实际锁定工作站。真实 ViewerEngine 配置往返及单会话监听退出触发已由 `viewer_display_control` 验证，仍需 Windows 实际锁屏/解锁后重连验收；主机硬崩溃不保证执行锁屏，API 接受请求也不代表已进入锁屏。RustDesk 对照：[选项](D:/proj/rustdesk/libs/base/protos/message.proto:678)、[结束处理](D:/proj/rustdesk/src/server/connection.rs:5306)。

1. **无人值守与安全桌面**：登录前 worker、桌面切换、SAS 已有代码；未完成实际 SCM 服务全链路、冷启动无人登录、注销再接入、锁屏/UAC、无头重启矩阵。服务同配置 Job 测试通过不等于 SCM 全链路通过。
2. **多人、跨网与设备组合**：本地 QUIC 多人和 GPU 视频回归已通过；双机/跨 NAT、TCP/WS、真实 IPv6、不同 GPU、多适配器、HDR/热插拔、高 DPI 仍缺覆盖。Go 普通测试通过，竞态检测仍有工具链阻塞。
3. **真实输入及音视频外设**：本地键鼠屏蔽测试使用 dry-run；触屏、精密触控板、摄像头/音频端点变化和双机 AEC 仍需实测。音频回归有历史偶发阈值失败，最近一次通过不等于根因已解决。
4. **虚拟显示驱动交付**：固定 ZIP/逐文件哈希、ACL/签名/篡改校验、本机安装已完成。再分发确认、正式包集成和干净机器安装/卸载/升级矩阵未闭环。只支持 usbmmidd；配置中 `vdd` 尚未实现，但不必为了功能对齐复制对方驱动品牌。
5. **普通会话期间的驱动 CLI 协调**：CLI 已阻止与隐私恢复租约竞争；普通桌面会话没有一直持有该租约，管理员直接 set-count/uninstall 仍缺统一会话占用检查。托盘有活动会话限制，不能据此视为所有入口都受保护。证据：[CLI 租约](D:/ds-chcast/src/host/virtual_display.cc:2389)、[托盘门控](D:/ds-chcast/host/main.cc:3072)。

## 不再列为完全缺失

- 多查看者、独立屏幕窗口、两个独立终端、手势、文件创建/重命名/删除、纯文件连接均有调用链。
- 虚拟屏已通过 1/2/4 个输出、1080p/1440p/4K 的 GPU 捕获/编解码、本地隐私开关/重复开启/撤权/断线/强杀主机/Job 关闭恢复。额外四个异常场景也通过，当前物理屏已恢复、临时虚拟屏已停用。完整边界见 [虚拟显示验证记录](D:/ds-chcast/docs/virtual-display-privacy-2026-11-05.md)。
- 最新相关 CTest 9/9，通过记录在 `build/align-vdisplay-validation-regression.log`；本次审核未重新执行硬件测试，不将历史结果记为本轮新跑。

## 本轮复核验证

- 确认本地对照提交仍为 `4b92080` / `873230e`，按当前未提交工作区核查；没有查询线上版本。
- 接续确认上一轮 C++ 全量 Release 和 Flutter Windows Release 构建均以 0 退出；Flutter 定向测试 25 项通过，analyze 无问题。日志为 `build/align-lock-full-build.log`、`build/align-lock-viewer-build.log`、`build/align-lock-flutter-test.log`、`build/align-lock-flutter-analyze.log`。
- 本轮重新运行协议、主机配置、安全、锁屏策略、输入屏蔽、多人会话、ViewerEngine 文件/终端及纯文件会话回归，**8/8 通过，25.54 秒**。日志：`build/align-functional-audit-regression.log`。这不是全套测试，也不替代实际 OS 锁屏、SCM、跨网及设备矩阵。
- 本轮只更新审核与进度文档，没有修改产品实现或驱动配置。

## 其他差异及建议顺序

TCP 点对点打洞策略尚未对齐，但已有 UDP 打洞/QUIC/TCP/WS 中继回退，不应写为“没有 TCP 连接”。企业组级远控准入、主机更新健康检查/回滚是我们的产品完善项，不混算 RustDesk Pro 或声称对方已验证领先。RustDesk 线协议互通、缺源码的 Web 核心不在本轮对齐目标中；跨平台按约定留待 Windows 阶段后。

建议继续补齐 **W06 多查看者及隐私组合边界、W01/W02 提权与系统会话选择**，并完成 W07 锁屏验收；再做 **W04/W05 独立辅助连接与终端恢复**。W03、W08–W14 按使用频度安排，其中视频格式继续服从硬件路径约束。无人值守/安全桌面和正式驱动交付属于发布前验收门槛，不应等所有次要功能做完再处理。严格零拷贝要求需单独跟踪 GPU 拷贝消除与资源生命周期改造。
