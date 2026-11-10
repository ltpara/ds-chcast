# RustDesk 全功能对照：客户端、被控主机与服务端（2026-11-07）

> **当前状态入口（2026-11-09 复核）**：本文件是协议 v49 基线的全产品审查快照。第一批主机 F01–F04 已完成；v50–v67 又关闭了多屏共享服务、全订阅者 QoS、按连接 ID 管理和编解码能力协商，并补齐单向权限、AV1 录制、大目录分页、空目录、按需文件剪贴板、进程级剪贴板监听、按设备偏好、防休眠、主机入站录制、VP9 4:4:4、远程提权和 G01 角色密钥分离，以及自定义画质百分比/帧率数值入口与显示器窗口位置恢复。下表中 C06/C08/C11–C14/C22–C23/C27–C28/C31–C32/C34/C37–C38/C48 等行和 G05–G08、G10、G11、G18、G20 的“缺口”描述已不代表当前状态；**发射端功能矩阵以 [发射端模块对比](host-rustdesk-module-comparison-2026-11-08.md) 为准，当前开发顺序以 [当前状态与计划](status-and-plan-2026-11-09.md) 为准**。本文件保留全产品视角的历史差异记录，剩余代码缺口为 G02/G03/G04、G13/G14、G19 的打印/角色互换/AV1 4:4:4 等；G10 Legacy 与 G11 查看端出站录制已于 2026-11-10 关闭，G12 管理权限颗粒度与自动化 API Token 主体也已落地（组范围暂只覆盖账号组与审计），G18 的分页、文件夹递归删除（v64）、隐藏项开关（v65）、多共享根/盘符（v65）与文件压缩（v66）已全部关闭，G15 的画质/帧率自定义数值入口与显示器窗口布局也已关闭（v67）。2026-11-11 按固定提交 `7423dce` 二次复核宿主侧：新增 C49–C51/G21–G23 三项 P3 差异（显示我的光标/白板、主机语音麦克风选择、会话期间移除壁纸）；截图（C40）由“已具备”改为“部分对齐”，原因是实现来源不同。

## 结论与范围

当前版本已经具备较完整的 Windows 远控、音视频、文件、终端、会合和中继链路。与 RustDesk 的主要差距集中在企业连接授权、集中策略、平台覆盖，以及若干功能组合的完整性，不能再用“hbbs/hbbr 基线已补齐”代表整个产品已对齐。

本次发现中，应最先处理的是**连接服务的普通使用凭据与后台管理凭据复用**；随后是账号到设备的连接 ACL、账号级会话权限、集中配置策略。客户端比较明确的缺口包括 AV1 录制、单向剪贴板/文件权限、更完整的文件浏览、Translate 键盘模式、自动录制和每设备偏好。

这是基于代码、协议、界面调用点和官方文档的功能审查。没有运行 RustDesk/本项目的双机性能对测，也没有在本轮重新运行全部测试。因此：

- **已具备**：找到协议、执行路径及入口；不表示任意设备、网络和权限组合均经过验收。
- **部分**：有实现，但覆盖范围或实际工作流存在明确差异。
- **缺失**：在当前实现中未找到对应产品能力，并核对了相邻模块/协议/入口。
- **待验**：代码存在，缺所需硬件、双机或公网环境的实测证据。
- **取舍**：现有设计有意不提供，仍属于用户能感知的功能差异，不擅自改成开发承诺。
- **工程项**：发布和部署可靠性建议；没有充分依据时，不声称 RustDesk 已解决同一个问题。

对照版本以 2026-11-07 查询官方 GitHub Releases API 的结果为准，排除 nightly：

| 对象 | 版本 / 提交 | 官方发布时间（UTC） | 核查范围 |
| --- | --- | --- | --- |
| 本项目 | `f48c9bce2b9f020d5bec9b499799ce0140f37db9`，协议 v49 | 本地审查基线 | Flutter、Windows C++、Go 服务端、测试和部署文件 |
| RustDesk 客户端 | **1.5.1**，`23e5bb471a040d62f0a6ad52591fb30784265b73` | 2026-11-01 16:13:27 | 固定 tag 的官方源码 |
| RustDesk Server OSS | **1.1.16**，`73523b31cfd25d77dee862e6fc9f5e1fb5e485ef` | 2026-07-20 22:25:36 | 固定 tag 的 hbbs/hbbr 官方源码 |
| RustDesk Server Pro | **1.8.7**，发布仓库 tag 对应 `7aaab2a4d38d6eacdcd6096834a4460c84b583dc` | 2026-10-01 02:21:46 | 官方功能文档和发布元数据；**未审查闭源服务实现** |
| RustDesk 官方文档 | `56f9310ca2c91f8e94b6a460290d1e94f62e0482` | 文档提交 2026-10-14 | 固定提交的英文功能文档，路径见文末 |

下文 `C` 表示官方客户端源码，`S` 表示开源服务端源码，`D` 表示官方文档。文件路径默认相对于本项目根目录；RustDesk 的路径均相对于上述固定源码快照。

## 一、客户端和主机功能矩阵

### 平台、入口与安装

| ID | 能力 | RustDesk 基线 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| C01 | Windows 控制端 / 被控端 | 原生桌面客户端及后台服务（C、D01） | **已具备**：Flutter 查看器、托盘主机、SYSTEM 服务分离 | `app/windows/`、`host/main.cc`、`host/service.cc` |
| C02 | Linux / macOS | 提供对应客户端；具体采集能力受平台限制（D01） | **缺失 / 取舍**：CMake 明确要求 MSVC/DXGI，查看器仅有 Windows runner | `CMakeLists.txt:14`、`app/windows/` |
| C03 | Android / iOS | Android 可控制/被控；iOS 不能被远程控制（D01） | **缺失**：无移动端产品及宿主桥接 | `app/` 平台目录、`app/lib/main.dart` |
| C04 | 浏览器控制端 | 有 Web；官方 V2 仍标注 Preview，自托管涉及 Pro 授权（D13） | **缺失**：Web 管理台存在，但没有浏览器远控；WS 载体不能替代浏览器客户端 | `server/internal/webadmin/`、`app/windows/viewer/` |
| C05 | 安装、服务、自启动、更新 | 桌面安装、后台运行、更新（C） | **已具备**：安装器、主机服务、签名更新与启动回滚；不等同于所有发布包已完成签名验收 | `server/internal/setup/`、`server/internal/updater/`、`host/service.cc` |
| C06 | 便携使用与会话中提权 | 便携启动提权、接受并提权、控制端请求提权（D12） | **已实现 / 待验**：v60 接入查看端请求、本地 UAC、父子进程校验、内存配置接管和重连；SYSTEM 便携辅助服务仍未实现，实际 UAC 与跨管理员账户待人工验收 | `host/elevation_handoff.cc`、`host/main.cc:781`、`src/net/quic.cc:3564` |
| C07 | 企业安装包 / 定制客户端 | MSI、批量脚本、Pro 定制客户端生成（C `res/msi/`、D10/D14） | **部分**：有 Windows EXE 打包和安装参数；未找到 MSI、品牌/配置生成台、配置锁定模板 | `tools/package-windows.ps1`、`server/cmd/chast-package/` |
| C08 | 多会话、多窗口 | 多连接与显示窗口（C） | **已实现 / 有限额**：独立 Flutter engine、最多 16 个窗口；主机默认 2、上限 8 个查看者，按显示源共享采集编码并支持每连接独立订阅/选屏；跨进程服务 worker 管理仍待验收 | `app/lib/session_window.dart`、`src/host/desktop_stream.h:18`、`src/host/host_server.cc:2388` |
| C09 | 角色互换 Switch Sides | 双端角色交接（C `src/server/connection.rs`） | **缺失 / 取舍**：两套程序分离，缺反向可达性检查、进程交接及一次性授权 | `app/windows/viewer/`、`host/`；旧任务 DEV-14 |
| C10 | 多语言、会话快捷操作 | 多种语言、键盘菜单、快捷操作（C/D11） | **部分**：查看器及主机中英双语，管理台主要为中文；已有常用快捷键，不能据此认定整个快捷键定制系统对齐 | `app/lib/l10n.dart:9`、`host/i18n*`、`server/internal/webadmin/assets/index.html:2` |

### 画面、显示器与输入

| ID | 能力 | RustDesk 基线 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| C11 | 多显示器、合并桌面、切屏 | 显示器选择与多屏查看（C） | **已实现 / 待验**：按显示源共享采集编码，连接可独立选屏/切屏，同屏复用；多 GPU、负坐标、旋转、高 DPI 与热插拔待实测 | `src/host/desktop_stream.h:18`、`src/host/desktop_stream.cc:1105`、`src/capture/merged_capturer.cc` |
| C12 | 跟随光标 / 前台窗口 | 对应会话选项（D11） | **已实现 / 待验**：显示跟随请求由订阅处理并返回结果，逐屏光标与窗口焦点独立；真机多屏坐标映射待验 | `include/chast/protocol.h` 的 `DisplayFollow*`、`src/host/desktop_stream.cc` |
| C13 | 远程分辨率、缩放和光标 | 分辨率、视图比例、光标缩放（C/D11） | **部分**：显示模式修改、适配/原始比例、原生光标已有；显示模式修改仍属主机级动作，真机 DPI 待验 | `src/capture/display_mode.cc`、`app/windows/viewer/remote_cursor.*` |
| C14 | 自定义画质和帧率 | 预设及自定义画质/FPS（D11） | **已实现 / 待真机**：预设画质、自定义画质百分比（10–100，主机按 percent/50 映射码率比）与自定义帧率（5–120）均有数值入口并按设备保存；各订阅连接参与全局 QoS，最新画质投票（预设或自定义）生效，共享编码取交集与多数票。双机联动与弱网观感仍待验 | `app/lib/widgets/remote_toolbar.dart:1921`、`app/lib/stream_prefs.dart:90`、`src/host/video_qos.cc:243`、`include/chast/protocol.h:2331` |
| C15 | H.264 / HEVC / AV1 | 具备多种编解码能力（C `libs/base/protos/message.proto`） | **已具备 / 待验**：三种硬件路径、VP9、OpenH264 软件回退和解码能力交集协商；不能把硬件不可用时回退当作 AV1 实机成功 | `src/codec/vpl_encoder.cc:255`、`src/codec/ffmpeg_encoder.cc`、`src/host/desktop_stream.cc:1105` |
| C16 | VP8 / VP9 / 4:4:4 | VP8、VP9、I444 按端点能力使用（C `libs/scrap/src/common/codec.rs`） | **部分完成**：v61 已提供 VP9 profile 1（I444）与全局色度协商；仍无 VP8，AV1 4:4:4 缺失 | `src/codec/vp9_encoder.cc`、`include/chast/protocol.h:1041`、`:1496` |
| C17 | 采集 / 编解码兼容性回退 | Windows 有 GDI 和软件编码等回退（C `src/server/video_service.rs:643`） | **已具备（方向一致）**：DXGI 失败切 GDI 上传路径，硬件编码连续失败回退 OpenH264；GDI 不是零拷贝路径，只作可用性兜底，驱动复位/RDP 进出仍待真机复验 | `src/capture/dxgi_capturer.cc:253`、`src/capture/detail.h:285`、`docs/software-encoder-2026-11-08.md` |
| C18 | HDR 桌面处理 | 平台相关采集/色彩处理（C） | **已有有限实现**：FP16 采集后归一到 BGRA8/NV12；不宣称 HDR10 端到端或与所有显示器等效 | `include/chast/capture.h:106`、`src/capture/hdr_pass.cc` |
| C19 | 隐私模式 / 阻止本地输入 | 隐私实现及输入控制（C） | **已具备 / 待验**：遮罩和虚拟屏方案、阻止输入、撤权/断线恢复；驱动拓扑仍待实机 | `src/win/privacy.cc`、`src/host/display_privacy.cc`、`tests/block_input_test.cc` |
| C20 | 虚拟显示器 / 无头使用 | Windows 虚拟显示相关能力（C） | **部分 / 待验**：usbmmidd 集成、安装预检、显示守卫已做；不是“完全没有”。泛化 `vdd` provider 仍返回未实现，驱动包/许可/真机验收是边界 | `src/host/virtual_display.cc`、`src/host/host_server.cc:3699`、`tools/fetch-usbmmidd.ps1` |
| C21 | 默认物理键、组合键、锁定键 | 支持的对端默认 Map；有原生键盘捕获（C `src/client.rs:3725`） | **已具备**：低级钩子、HID、AltGr 过滤、按键释放和锁定键同步；此前按默认模式对齐的方向正确 | `app/windows/viewer/keyboard_capture.cc`、`src/win/input.cc`、`tests/keyboard_test.cc` |
| C22 | Legacy / Translate 模式 | 桌面菜单提供 Map、Legacy、Translate；Translate 标注 beta（C `flutter/lib/desktop/widgets/remote_toolbar.dart:2615`） | **已实现 / 待实测**：Map（默认）、本地输入法/Translate 方向（Unicode 提交）与 v63 Legacy（发送 Windows 虚拟键码，主机注入同名按键，由主机布局和输入法解释）均已接入；非 US 布局/AltGr/死键/其他 IME/安全桌面待验 | `app/windows/viewer/keyboard_capture.cc`、`src/win/input.cc`、`docs/local-keyboard-ime-2026-11-09.md`、`docs/keyboard-legacy-mode-2026-11-10.md` |
| C23 | 中文 / IME / 发送文本 | 依模式和平台处理字符输入（C） | **已实现 / 待验**：本地 IME 组合输入经 TextInput 发送 Unicode，保留手动“发送文本”入口；微软拼音与双 Flutter 窗口/QUIC 已验证，其他输入法与双机目标应用待验 | `docs/local-keyboard-ime-2026-11-09.md`、`src/win/input.cc` |
| C24 | 鼠标、相对模式、触摸/笔、触控板 | 相应输入能力，平台有差异（C） | **已具备 / 待验**：相对鼠标、左右键互换、滚轮反向、多触点、压感、Pan/Zoom；实际触控板、笔和双机布局仍待验 | `app/lib/remote_pointer.dart`、`src/win/input.cc`、协议 v43/v48 |
| C25 | 每设备会话偏好 | `PeerConfig::load(id)` 保存对端设置（C `src/client.rs:3043`） | **已具备**：`ui.json` 根键保留全局默认，新增 `peers.<id>` 覆盖画质/编码/FPS/全色、适配/统计/滚轮/左右键/键盘模式；设备键为服务器 host ID 或直连地址，独立显示窗口同样按该键加载。文件浏览器排序/目录与终端字号仍为全局，属本地面板状态 | `app/lib/prefs_store.dart:36`、`app/lib/stream_prefs.dart:45`、`app/lib/view_prefs.dart:66`、`app/lib/pages/session_page.dart:336` |

### 剪贴板、文件与录制

| ID | 能力 | RustDesk 基线 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| C26 | 文本、HTML、RTF、图片剪贴板 | 多格式同步（C `src/clipboard.rs`） | **已具备**：异步读写、失败重试、基线、撤权取消已修复；系统原生调用的取消边界仍存在 | `src/clipboard/clipboard_worker.cc`、`docs/clipboard-async-fix-2026-11-07.md` |
| C27 | 共享事件驱动剪贴板 | 全局监听器、多订阅者（C `src/clipboard.rs:889`） | **已具备 / 待验**：进程级单监听（消息窗口 + `AddClipboardFormatListener`、原子代次、订阅者按 id 注册，首订阅启动/末退订停止）；原生后端由 `WM_CLIPBOARDUPDATE` 代次驱动并保留 5 s 兜底，合成后端或监听不可用时仍 500 ms 轮询；多窗口/多主机复制的人工复核待做 | `include/chast/clipboard_listener.h`、`src/clipboard/clipboard_listener.cc`、`app/windows/viewer/viewer_engine.cc`、`src/host/host_server.cc`、`tests/clipboard_listener_test.cc` |
| C28 | 文件复制粘贴 | Windows cliprdr 提供延迟请求文件内容（C `libs/clipboard/src/windows/wf_cliprdr.c`） | **已实现 / 待验收**：v59 虚拟文件描述 + `IStream` 按需范围读取，v58 保留空目录，复制不再立即全量搬运；Explorer/Office 人工操作、其他虚拟文件来源和跨网吞吐待验 | `include/chast/clipboard.h`、`src/clipboard/file_transfer.cc`、`include/chast/protocol.h:435` |
| C29 | 单向剪贴板 / 单向文件 | 有阻止被控端→控制端的独立策略（D11） | **已实现**（v57）：本地双向/单向/禁止策略覆盖文本、图片和文件剪贴板，禁止发送的一端停止本地读取；文件剪贴板权限独立于普通文件传输 | `src/host/host_server.cc`、`include/chast/protocol.h:1265`、`docs/clipboard-direction-2026-11-09.md` |
| C30 | 初始剪贴板同步选择 | 可选择连接时同步控制端已有内容（D11 `sync-init-clipboard`） | **缺失选项 / 取舍**：当前首次有效快照只建基线，默认保护历史内容；可增加明确的单次或持久化选择 | `docs/clipboard-async-fix-2026-11-07.md` |
| C31 | 完整远程文件系统浏览 | Windows 盘符枚举、目录浏览、隐藏文件选项（C `libs/base/src/fs.rs:53`） | **已具备 / 按授权**：隐藏文件选项已由 v65 补齐（默认隐藏、按设备持久化、递归下载沿用视图）；v65 多共享根允许主机把最多 16 个盘符根或绝对目录加入文件共享，观看端在顶层看到虚拟别名。仍是“主机显式授权”模型，不自动枚举全部盘符 | `include/chast/protocol.h:802`、`src/clipboard/file_transfer.cc`、`docs/hidden-files-2026-11-10.md`、`docs/multi-share-roots-2026-11-10.md` |
| C32 | 大目录浏览 | RustDesk 目录模型返回条目列表（C `libs/base/src/fs.rs`） | **已实现**（v56）：游标分页、“加载更多”和同页重试，递归下载自动读取后续页；万条目录可分页访问 | `include/chast/protocol.h:267`、`:775`、`docs/file-pagination-2026-11-09.md` |
| C33 | 上传下载、文件夹、批量、取消、断点续传 | 常规文件管理工作流（C） | **已具备 / 有限额**：已有目录队列、批量选择、续传/取消；单文件协议上限 16 GiB，主机可设更小限制 | `include/chast/protocol.h:637`、`app/lib/widgets/file_browser_panel.dart`、`src/clipboard/file_transfer.cc` |
| C34 | 递归删除、文件压缩传输 | `FileRemoveDir`/递归文件读取、压缩块（C `libs/base/protos/message.proto:409`、`libs/base/src/fs.rs:1106`） | **已具备**：v64 `FileOperation::RemoveTree` 预检整棵树，拒绝 reparse point/只读条目后自底向上删除；观看端文件夹删除与批量删除已接入确认和递归操作。v66 为 CFIL 增加可协商 v2 块流（9 字节块头、zstd level 1、按块原始回退，压缩仅用于非剪贴板且非已压缩容器的大文件），真实 QUIC 回环 1 MiB 文本仅 1776 wire 字节。递归删除和压缩均已有真实 QUIC 回环覆盖 | `include/chast/protocol.h`、`src/clipboard/file_transfer.cc`、`docs/recursive-remove-2026-11-10.md`、`docs/file-compression-2026-11-10.md` |
| C35 | 文件拖入和资源管理器衔接 | 桌面文件交互（C） | **已有部分**：WM_DROPFILES、本地上传选择、完成后打开/定位；不宣称等价于全部 OLE 双向拖放 | `app/windows/viewer/viewer_bridge.cc:2672`、`app/lib/widgets/file_browser_panel.dart` |
| C36 | 手动录制、分段、归档 | 支持会话录制及多种码流（C） | **已具备**：H.264/HEVC 原码流 MP4、尺寸/编码切换分段、可选账号上传；上传和录制是两项独立能力 | `tools/mp4_recorder.h`、`app/lib/pages/session_page.dart:503`、`server/internal/api/recordings.go` |
| C37 | AV1 录制 | AV1→WebM 录制路径（C `libs/scrap/src/common/record.rs:168`） | **已实现**（G06）：录制器支持 AV1/VP9 合法封装，编码切换分段；`mp4_recorder_test` 与入站录制接线已覆盖 | `src/host/stream_recorder.h:20`、`tests/mp4_recorder_test.cc` |
| C38 | 主机自动录制 / 查看器自动录制 | 入站自动录制、出站自动录制、隐藏停止按钮选项（C、D11） | **已完成主体**：入站 `record_incoming` 默认关闭，共享压缩数据异步写 MP4，同源多人只录一份；查看器出站自动录制默认关闭（对照 `allow-auto-record-outgoing`），含 512/64 MiB 容量守卫、失败提示和可选保留策略（默认保留全部）。`hide-recording-button` 是 RustDesk 受管客户端设置，归受管策略（G04）而非录制链路 | `include/chast/host_settings.h:108`、`src/host/host_server.cc:2402`、`docs/host-recording-2026-11-09.md`、`docs/viewer-auto-record-2026-11-10.md` |

### 主机控制、协作与专用会话

| ID | 能力 | RustDesk 基线 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| C39 | 系统声音、麦克风、语音通话 | 音频传输及通话协商（C） | **已实现 / 待验**：注册表级 `SharedLoopback` 按连接订阅/权限/发送，发布者静音或拥塞不影响其他有权限者；Opus、QUIC DATAGRAM、AEC、呼叫状态已有；双机听感、热插拔和多人争用未验。主机语音麦克风可在设置/托盘选择，缺失设备回退默认通信设备（G22），真实设备切换待验 | `src/host/host_server.cc:2689`、`src/audio/`、`docs/shared-video-qos-rustdesk-2026-11-08.md` |
| C40 | 聊天与截图 | 客户端会话功能（C） | **部分对齐**：聊天 UI/协议已具备；截图由观看端把最新解码帧写成 PNG，不是 RustDesk 的主机端 `ScreenshotRequest`，分辨率和可截取范围受当前订阅/解码限制。若要求按显示器全分辨率截图，需要新增主机截图消息 | `app/lib/widgets/chat_panel.dart`、`app/windows/viewer/viewer_engine.cc:1423`、`src/win/png_writer.cc` |
| C41 | 远程终端、持久终端 | 终端服务、重连、输出回放（C） | **已具备 / 有限额**：两标签，100 个持久服务、1 小时空闲回收、1 MiB 回放；持久化默认关闭 | `include/chast/protocol.h:1564`、`src/host/terminal_relay.cc`、`app/lib/widgets/terminal_tabs.dart` |
| C42 | TCP 隧道 / RDP 入口 | TCP tunneling 与 RDP（C） | **已具备**：最多 8 规则及目标白名单、mstsc 启动；这不是通用 VPN/UDP 转发，也不把后者计为 RustDesk 缺口 | `include/chast/protocol.h:1483`、`src/net/port_forward_client.cc`、`app/lib/rdp.dart` |
| C43 | 摄像头及独立会话类型 | 桌面、文件、终端、摄像头、隧道（C） | **已具备 / 待验**：独立模式无需桌面流；摄像头完整媒体链路仍需真实设备 | `include/chast/protocol.h:1061`、`src/host/camera_relay.cc`、`app/lib/widgets/camera_panel.dart` |
| C44 | 重启、锁屏、Ctrl+Alt+Del、断开后锁定 | 主机操作（C） | **已实现 / 待验**：桌面连接和跟随会话都消费主机动作请求并返回权限/结果；SAS 单独处理，断开后锁定按最后观看者退出执行；真实安全桌面动作待受控验收 | `src/win/sas.cc`、`src/host/host_server.cc:2202` |
| C45 | OS 登录 / 自动登录 | 登录相关字段及自动登录逻辑（C `src/client.rs:3031`、`:4450`） | **部分**：用户手动提供密码，在登录/锁屏界面输入；没有完整账号选择、保存凭据和自动登录策略。不能把它整体标成自动登录完成 | `app/lib/pages/session_page.dart:2005`、`include/chast/protocol.h:1823` |
| C46 | Windows 会话选择 | WTS/RDP 会话选择（C） | **已实现 / 待验**：RDP-only 主机和登录代次防 ID 复用已支持，切换请求绑定来源工作进程和目标登录；真实多用户切换仍需受控验收 | `host/service.cc`、`src/host/host_server.cc:331`、`docs/windows-session-selection-2026-11-09.md` |
| C47 | 远程打印 | 打印服务和驱动适配（C `src/server/printer_service.rs`） | **缺失 / 平台前置**：无打印协议/客户端/主机链路，仍需驱动分发及签名方案 | 旧任务 PLAT-02；当前协议和主机模块清单 |
| C48 | 活跃会话防休眠 | 入站/出站保持唤醒选项（D11） | **已实现 / 待验**：默认开启本地 `keep_awake`，桌面连接请求系统+显示器、文件/终端只请求系统，按连接释放，待审批不持有；真实电源策略与长时间会话待验 | `include/chast/host_settings.h:191`、`src/win/power_request.cc:35`、`docs/session-power-2026-11-09.md` |
| C49 | 显示我的光标 / 白板 | `show_my_cursor` 选项、主机 `src/whiteboard` 透明覆盖层、Flutter 工具栏/快捷键（C `src/server/connection.rs:4760`、`flutter/lib/desktop/widgets/remote_toolbar.dart:2730`） | **已实现（光标部分，P3，G21）、已构建、自动化通过**：v68 `ShowCursor` 携带虚拟桌面坐标与可见标志，主机在独立线程创建分层、点击穿透、不激活的覆盖层绘制本地指针，断线/关闭隐藏；查看端工具栏开关按设备保存并 33 ms 节流发送。不注入点击。RustDesk 式白板绘制未实现；覆盖层真实多屏/混合 DPI 目视位置待人工验证 | `include/chast/show_cursor.h`、`src/win/show_cursor.cc`、`include/chast/protocol.h:681`、`app/lib/pages/session_page.dart:2306`、`tests/protocol_test.cc:419` |
| C50 | 主机语音麦克风选择 | 主机设置/连接管理可为语音呼叫选择本机麦克风（C `src/server/audio_service.rs:62`、`flutter/lib/common/widgets/audio_input.dart` 的 `isCm/isVoiceCall`） | **已实现（P3，G22）、已构建、自动化通过**：`host.ini voice_microphone=`（空=系统默认）、设置页下拉/刷新和托盘“上行语音麦克风”子菜单；呼叫开始时读取，固定设备打不开记录日志并回退默认通信设备，已在通话中不切换。观看端麦克风选择保持不变。本机无物理麦克风，真实录音与设备切换待人工验证 | `include/chast/host_settings.h`、`host/main.cc:2928`、`src/host/host_server.cc`（语音呼叫设备选择与回退）、`tests/host_voice_test.cc` |
| C51 | 会话期间移除壁纸 | `allow-remove-wallpaper` 在入站授权后移除主机壁纸并在会话结束恢复（C `src/server/connection.rs:2115`、`src/platform/windows.rs` WallpaperRemover） | **已实现（P3，G23，优化项）、已构建、自动化通过**：默认关闭的 `remove_wallpaper` 与托盘开关；桌面会话开始时读取 SPI 壁纸路径，备份到 `<stem>.wallpaper` 后用空路径清屏，会话结束恢复；下次启动恢复上次异常退出遗留的备份。备份/冲突拒绝/会话生命周期有自动化，真实壁纸视觉切换和断电恢复待人工验证；不改变零拷贝与 QoS | `include/chast/wallpaper.h`、`src/win/wallpaper.cc`、`src/host/host_server.cc`（会话生命周期）、`tests/host_voice_test.cc`、`tests/host_settings_test.cc` |

## 二、连接服务和企业管理矩阵

| ID | 能力 | RustDesk 版本边界 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| S01 | ID 注册、身份绑定、在线和保活 | OSS hbbs（S） | **已具备**：永久 ID/公钥绑定、签名元数据、租约、批量在线、ID 迁移 | `server/internal/registry/`、`server/internal/api/api.go` |
| S02 | NAT 探测、打洞、IPv6、UPnP | OSS 会合基础；具体端到端能力还依赖客户端版本（S/C） | **已具备 / 待验**：双栈候选和代理路径存在；公网、对称 NAT、v6-only/双栈回退仍需实测 | `server/internal/rendezvous/`、`server/internal/agent/`、`server/internal/upnp/` |
| S03 | 加密中继、TCP/WS、代理 | OSS hbbr / 客户端（S/C） | **已具备 / 实现不同**：转发 QUIC 数据报，TCP/WS 作为载体；HTTP(S)/SOCKS 代理路径已做 | `server/internal/relay/`、`server/internal/carrier/`、`server/internal/netproxy/` |
| S04 | 传输协议兼容 | RustDesk 自有协议及多种直连/中继方式（S/C） | **有意不兼容**：CHAST JSON/QUIC，不是可直接替换 hbbs/hbbr 的 RustDesk 服务器；互通不作为当前开发任务 | `server/internal/wire/wire.go`、`include/chast/protocol.h:60` |
| S05 | 独立中继、健康、限速和封禁 | OSS hbbr；Pro 增加管理功能（S/D09） | **已具备**：独立节点、租约、健康、单会话/总带宽和名单 | `server/internal/relaypool/`、`server/internal/relay/policy.go` |
| S06 | 地理就近中继 | Pro GeoLocation 路由（D09） | **缺失**：当前黏性优先+健康轮询，无按两端区域/延迟选节点；审计 GeoIP 不是路由 GeoIP | `server/internal/relaypool/pool.go:270`、`server/internal/geoip/` |
| S07 | 服务器列表更新 / 故障回退 | 会合配置及客户端选择（S/C） | **已具备**：序号化服务器列表和粘性回退；不等价于共享运行状态的多活集群 | `server/internal/configpush/`、`server/internal/agent/servers.go` |
| S08 | 账号、密码、封禁、会话令牌 | Pro（D02） | **已具备**：账号、短访问令牌、刷新轮换、重放撤销、设备会话管理 | `server/internal/account/`、`server/internal/api/account.go` |
| S09 | 2FA、找回、安全密钥 | Pro 文档列 Email/TOTP；本项目另有 WebAuthn（D02） | **已具备 / 部分待验**：TOTP、恢复码、Email/SMS、WebAuthn；不把 WebAuthn 反向推断为 RustDesk 也有 | `server/internal/webauthn/`、`app/lib/security_key.dart`、相关 account/API |
| S10 | OIDC 单点登录 | Pro（D02） | **已具备**：授权码+PKCE/JWKS、浏览器登录、身份链接；部署为一个 Provider 配置 | `server/internal/oidc/`、`server/internal/api/oidc.go`、`app/lib/server_api.dart` |
| S11 | LDAP / AD 直连认证 | Pro LDAP；官方文档明确尚不支持 LDAP 组同步（D07） | **缺失**：仅 OIDC 等认证，无 LDAP bind/search/首次登录建号。经外部 IdP 对接 AD 可作为现有替代，不等于内建 LDAP | `server/internal/oidc/`、`server/internal/api/api.go`、`server/cmd/chast-server/main.go` |
| S12 | 普通连接凭据与管理凭据隔离 | Pro 区分普通账号、Admin Role、API Token（D03/D06/D10） | **已实现角色密钥分离（G01）**：新增部署级 link/host 角色密钥；lookup/lookupBatch 与 UDP/TCP punch 只认 link key，注册与主机签名审计只认 host key，管理台/metrics/console 等仍只认 server key；角色 key 为空时回退旧 server-key 语义，管理 key 始终可作根凭据。回环与测试证据见 G01 | `server/internal/api/api.go`、`server/internal/api/console.go`、`server/internal/rendezvous/`、`app/lib/server_settings.dart`、`host/tunnel.cc` |
| S13 | 用户/用户组→设备连接 ACL | Pro Access Control（D03） | **缺失**：账号组仅约束后台操作/审计；lookup/punch 未按登录账号和设备组判定。移出组/封禁账号不是桌面连接资格撤销 | `server/internal/account/groups.go:13`、`server/internal/api/groups.go:114`、`server/internal/rendezvous/rendezvous.go:69` |
| S14 | 用户级会话操作角色 | Pro Control Role，区别于连接 ACL（D04） | **缺失**：主机本地权限和审批存在，但 Hello 无账号角色凭证；不能集中规定“甲可看、乙可传文件、丙可开终端” | `include/chast/protocol.h:1335`、`src/host/host_security.cc` |
| S15 | 批量配置策略 / 下发 | Pro Strategy（D05） | **缺失**：会话内远程改 14 项开关已做，但没有设备/用户/组策略对象、版本、分配、状态及批量同步 | `include/chast/protocol.h:2103`、`server/internal/configpush/configpush.go:22` |
| S16 | 细粒度后台角色 | Pro Admin Role，可按资源动作和范围授权（D06） | **已完成主体（G12，2026-11-10）**：readonly/operator/admin 动作阶梯 × devices/audit/accounts/groups/recordings/ops 资源集合，账号组范围叠加；没有自定义命名角色对象，组范围暂只覆盖账号组与组审计。见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md) | `server/internal/api/console.go:59`、`server/internal/api/groups.go:116`、`server/internal/account/groups.go` |
| S17 | 自动化 API Token / 委派 | Pro 可选 Device/Audit/User/Group 等权限的 Token（D10） | **已实现（G12，2026-11-10）**：独立 `chast_api_` 机器令牌，资源集合、组范围（组/审计）、1 分钟–365 天、每用户 20 个、即时撤销、owner 降级收窄、封禁拒绝、最近使用与创建/撤销审计。边界：无 webadmin 管理界面，设备/录制尚不支持组范围。见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md) | `server/internal/account/api_tokens.go`、`server/internal/api/api_tokens.go`、`server/internal/api/api.go:338` |
| S18 | 设备管理、别名、标签、离线资产 | Pro 控制台（D10） | **部分**：清单、搜索、分页、元数据、别名标签、重置已有；集中禁用设备和所有者连接授权随 S13 补齐 | `server/internal/api/identities.go`、`server/internal/registry/registry.go:49` |
| S19 | 地址簿同步、分组、共享 | Pro（D08） | **已具备核心**：OCC、离线意图、嵌套组、逐账号共享/组范围；不要与账号组的连接权限混淆 | `server/internal/account/store.go`、`app/lib/address_book_sync.dart`、`app/lib/pages/shared_books_dialog.dart` |
| S20 | 地址簿共享密码 / 云端凭据 | Pro 个人密码同步、条目/簿级共享密码（D08） | **缺失 / 取舍**：密码留在本机 DPAPI，服务器 Peer 无密码字段；换设备/共享给同事后不能获得相同免输入体验 | `server/internal/account/store.go:183`、`app/lib/viewer_bridge.dart:1880` |
| S21 | 地址簿备注、回收站、组授权对象 | Pro 备注、回收站、User/Group/Everyone 共享（D08） | **部分**：当前有删除 tombstone，不是可恢复回收站；Peer 无 note；共享主要按账号，缺独立组织组/Everyone 授权模型 | `server/internal/account/store.go:183`、`:231`、`app/lib/pages/shared_books_dialog.dart` |
| S22 | 账号、连接、文件审计 | Pro（D02） | **已具备**：查看器账号上报与主机签名上报、双侧关联/去重、查询、导出、归档；真实跨网可信性和运维保留策略需环境验收 | `server/internal/api/host_audit.go`、`server/internal/account/audit.go`、`server/internal/auditarchive/` |
| S23 | 录制存储和配额 | 不将本项目实现倒推为 RustDesk 同等能力 | **本项目已有扩展**：账号录制上传、下载、删除、校验、保留/配额；缺强制产生录制的上游链路见 C38 | `server/internal/api/recordings.go`、`server/internal/account/recordings.go` |
| S24 | Web 管理台 | Pro（D10） | **已具备基础 / 入口不足**：设备/用户/组/审计/录制等页已有；账号密码/TOTP 或 server-key 登录，未接 OIDC/WebAuthn 登录 UI | `server/internal/webadmin/assets/index.html:17`、`server/internal/webadmin/assets/app.js:1596` |
| S25 | TLS、证书、mTLS、ACME、限流 | 部署方式有差异，不强求实现相同 | **本项目已有**：原生 HTTPS/mTLS、热加载、ACME、服务身份固定、持久化登录锁定等 | `server/internal/netutil/`、`server/internal/acmecert/`、`server/internal/authlimit/`、`app/lib/server_identity.dart` |
| S26 | 数据库、迁移、可观测性 | OSS/Pro 各自部署能力，不推断闭源内部 | **本项目已有**：SQLite/PG、迁移、日志轮转、Prometheus、OTLP；PG 不自动解决运行状态多活 | `server/internal/dbmetric/`、`server/internal/tracing/`、`server/internal/schemamig/` |
| S27 | LAN 发现、Wake-on-LAN | 客户端发现/局域网辅助（C） | **已具备并有扩展**：LAN 发现、直接唤醒及带显式目标-代理策略的跨子网唤醒；真实网卡唤醒仍待验 | `server/internal/lan/`、`server/internal/wake/`、`server/cmd/chast-server/wake_policy.go` |

## 三、可执行的缺口优先级

优先级是本次建议，不更改已有的“不实现”决策。P0 指在多人部署前应先解决的权限边界；P1 为日常体验或团队管理的主要缺口；P2 为按产品目标选择的扩展。

| 编号 | 优先级 | 问题与影响 | 建议的完成标准 |
| --- | --- | --- | --- |
| G01 | **P0（配置私服并向普通用户发连接密钥时）** | **连接密钥可同时管理服务器**：S12 的 server-key 被 lookup、punch 和后台共用，违背普通使用者与管理员分离。**2026-11-09 已实现角色密钥分离**（见下文状态） | 普通账号/短期设备连接凭证可 lookup/punch；注册凭据、连接凭据、管理凭据独立；普通查看器凭据对建号、重置身份、审计/录制越权操作返回拒绝。覆盖 HTTP、UDP、TCP/WS 与续期 |
| G02 | **P1，企业准入前必做** | **连接 ACL 未落到连接链路**：S13 的组管理不能限制谁连谁，账号封禁也不是主机本地密码失效。服务端 v10 设备策略、签名回执、lookup 准入和账号换 60 秒票据端点已落地，管理台设备策略页按版本匹配显示回执，注册响应已下发策略且 Go 代理已解析并经 `HostInfo`/`chast-tunnel` 事件上报、落盘共享 `<stem>.policy.json`，C++ 对受管设备的新会话失败关闭，并已形成签名回执闭环（主机确认文件 + agent 设备密钥异步上报），C++ 票据校验库已通过冻结黄金向量与错误输入测试，并接入 `HostSecurity`：主机设置固定 issuer/kid/公钥与设备 ID 后，受管设备只接受验签通过、版本匹配且未重放的票据，权限取票据 ∩ 上限 ∩ 本机默认 ∩ 请求；v62 已让客户端在 Hello 携带票据、lookup 返回票据并支持 20 秒续租与运行中撤权；受管主机缺票据失败关闭，直连同样经 HostSecurity 守门；撤销推送、查看器账号 token lookup（受限设备可准入）、独立传输凭证、管理应急访问查看器入口与真实链路验收（2026-11-11）均已完成；管理台只提供应急审计筛选，真主机进程整链仍待验收，因此连接 ACL 的端到端结论仍需按 G02 验收矩阵执行 | 独立服务端签发、主机仅持公钥；账号/组→设备策略；最小受管同步/落盘/确认前置到 G02，所有入口共同守门。新 lookup、旧票据和存量会话均验证撤销，最长 65 秒；独立短期传输凭证，不允许回退绕过。见 [修订设计](access-control-design-2026-11-09.md) |
| G03 | P1 | **账号级会话权限角色缺失**：S14 只有主机本地授权 | 沿用 G02 签发/租约；票据上限 ∩ 设备策略 ∩ 本地策略 ∩ 用户同意，覆盖所有敏感入口及正在运行操作的撤销；最多 65 秒失效。不需要让中继解密桌面数据 |
| G04 | P1 | **集中策略缺失**：S15 的“配置下发”目前仅会合地址列表；会话内改单机设置不能代替设备管理 | 策略对象、分配范围、优先级、版本、同步确认和失败可见；用户设置与策略的冲突行为有明确定义。RustDesk Strategy 本身也不等于所有值永久强制锁定 |
| G05 | P1 | **缺单向剪贴板/文件权限**（C29）**2026-11-09 已实现**（v57） | 分离每个方向；主机在普通下载、剪贴板文件、上传、暂停恢复等所有入口执行，不能仅禁用 UI。验证：方向专项、完整会话回归、实际观看端双向文件收发 |
| G06 | P1 | **AV1 与录制不可组合**（C37）**2026-11-09 已实现** | 增加 AV1 合法封装，或先提供明确的编码协商/选项互斥；覆盖开始录制、录制中切 AV1、回切及分段文件可读验证，不改变零拷贝采集。验证：`mp4_recorder_test` 与主机入站录制接线 |
| G07 | P1 | **大目录条目不可达**（C32）**2026-11-09 已实现**（v56 分页） | 加分页/游标或流式列举；10,000 项目录能逐页访问且递归下载完整；测试目录在分页期间变化的行为。验证：真实 QUIC 两页 1030 项、1025 空目录 + 4 文件完整落盘 |
| G08 | P1 | **文件剪贴板体验不等价**（C28）**2026-11-09 已实现**（v58/v59） | 先保留空目录、展示数量/大小限制；若追求 RustDesk 体验，再实现按粘贴/读取请求传内容，避免仅复制即全量下载；覆盖取消、断线、覆盖源文件。验证：空目录四种场景、虚拟文件 `IStream` 按需读取；Explorer 人工操作仍待验收 |
| G09 | P1 | **剪贴板仍按会话轮询**（C27；进程级监听已补齐） | 已实现进程级系统变更通知与订阅者管理：原生后端事件驱动、5 s 兜底，合成后端保持 500 ms；既有异步读写与基线规则不变。**2026-11-10 真实 GUI 复核通过**：同进程双会话连 62670/62671，一次本地变更每个存活窗口恰好发送 1 条（关闭首页后仍按窗口独立，关闭一个会话后仅剩窗口发送，无跨会话串发）；关闭首个 engine 首页后两会话继续串流且无 WER/转储。剩余：真实双机接收方向（单机共享剪贴板自回声抑制，收 0）
| G10 | P1 | **键盘缺 Translate / Legacy 选择**（C22/C23）**2026-11-10 已实现（v63）** | Map 保持默认，本地输入法/Translate 方向已实现（主/子窗口模式切换、Unicode 发送、顺序/取消/回环过滤）；Legacy 模式经 `InputKind::LegacyKey` 发送虚拟键码，主机按 VK 注入并同步修饰键/锁定键。剩余：非 US 布局、AltGr、死键、其他 IME、安全桌面输入实测 |
| G11 | P1 | **已完成（2026-11-10）** | 入站 `record_incoming` 默认关闭，共享压缩数据异步写 MP4，H.264/HEVC/AV1/VP9、分段与积压关键帧恢复；查看端出站自动录制默认关闭，含容量守卫、失败提示与可选保留策略；集中留档由受管主机负责并报告失败。真实会话与磁盘满场景待人工验证，见 [查看端自动录制](viewer-auto-record-2026-11-10.md) |
| G12 | P1 | **已完成主体（2026-11-10）** | 资源/动作 scope、组范围（账号组、组审计、设备与录制）、独立机器令牌的创建/到期/撤销/最近使用/审计已实现；控制台委派与普通连接权限保持分离；webadmin“API 令牌”页已接线并完成本地真实浏览器冒烟。剩余：真实部署升级与 Postgres 验收。见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md) |
| G13 | P2（跨地区部署可升 P1） | **无就近中继**（S06） | 在现有健康/黏性基础上按区域或两端实测延迟选择；节点失效回退；用真实多地区网络验收收益 |
| G14 | P2（AD 用户可升 P1） | **LDAP 认证缺失**（S11） | LDAP/LDAPS 连接测试、过滤、登录、首次建号、账号停用语义；不把 RustDesk 文档也未支持的 LDAP 组同步当作必须对齐项 |
| G15 | P2 | **固定画质选项和显示器窗口布局**（C14/C25）**2026-11-10 已完成** | 预设画质旁新增自定义画质百分比（10–100，协议 v67 的 `quality_percent`，主机 QoS 以最新投票在预设/自定义间切换）与自定义帧率（5–120），全部按设备保存；显示器子窗口按“设备 + 显示序号”记住物理像素位置/尺寸，与任何显示器都不相交的矩形被忽略。剩余为双机/多显示器人工目视验收 |
| G16 | P2 | **地址簿高级协作不全**（S20/S21） | 先补备注、回收站和组织组共享；云端密码同步另做密钥/撤销设计，不直接上传现有 DPAPI 密文或明文 |
| G17 | P2 | **临时支持流程不全**（C06/C09/C45） | 分别设计便携提权、角色交接、OS 登录便利功能；要求本地同意、内存凭据传递和可恢复的交接，不沿用“不装服务就绝对做不到”的假设 |
| G18 | P2 | **完整文件管理范围和操作**（C31/C34）**分页（v56）、递归删除（v64）、隐藏项（v65）、多共享根（v65）、文件压缩（v66）均已完成** | v64 `RemoveTree` 先预检 reparse point/只读条目再自底向上删除，真实 QUIC 回环通过；v65 `FileListRequest` 增加 flags 字节，主机分页前过滤 `FILE_ATTRIBUTE_HIDDEN`，观看端开关按设备持久化、递归下载沿用视图；多共享根通过 `host.ini` 的 `file_share_roots` 提供最多 16 个绝对目录（盘符根别名 `C`、重名追加 ` (2)`），接收文件夹自动并入，列表/下载/递归下载/上传/操作统一拆别名后走原沙箱；v66 的 CFIL v2 对不小于 4 KiB、非剪贴板且非已压缩容器的文件按块 zstd 压缩，收益不足回退原始块，续传仍走 v1。真实 QUIC 回环：1 MiB 文本 1776 wire 字节，300 KiB 随机数据 307245 wire 字节（5 个原始块多 45 字节）。见 [文件夹递归删除](recursive-remove-2026-11-10.md)、[隐藏文件浏览选项](hidden-files-2026-11-10.md)、[多共享根与盘符](multi-share-roots-2026-11-10.md)、[文件传输压缩](file-compression-2026-11-10.md) |
| G19 | P2 | **平台和兼容性覆盖**（C02–04/C16–17/C47）**VP9 4:4:4 已完成（v61）** | 分开评估 Web、Linux 查看器、移动端、其他主机平台、打印、4:4:4及软件回退；均为独立项目。VP9 I444 已实现，AV1 4:4:4、远程打印和 Switch Sides 仍缺 |
| G20 | P2 | **防休眠和界面入口一致性**（C10/C48/S24）**防休眠已完成（2026-11-09）** | 明确入站/出站保持唤醒，退出恢复电源请求；管理台登录接已有认证后端，语言与错误状态一致。已实现默认开启的 `keep_awake`，桌面请求系统+显示器、文件/终端只请求系统，按连接释放，待审批不持有；管理台入口一致性仍按各自模块验收 |
| G21 | P3 | **显示我的光标/白板**（C49）**光标部分已完成（2026-11-11，v68）** | v68 `ShowCursor` + 主机点击穿透覆盖层 + 查看端工具栏/节流；断线清理，不注入点击。自动化通过；白板绘制未实现，真实多屏/DPI 目视位置待验 |
| G22 | P3 | **主机语音麦克风不能选择**（C50）**已完成（2026-11-11）** | `voice_microphone` 设置 + 设置页/托盘入口；呼叫开始时读取，设备缺失回退默认通信设备并记日志。设置/合成采集自动化通过；无物理麦克风，真实听感待验 |
| G23 | P3 | **会话期间移除壁纸缺失**（C51）**已完成（2026-11-11）** | 默认关闭的托盘开关；会话开始备份并清屏、结束恢复、下次启动恢复异常备份。设置/生命周期自动化通过；真实壁纸视觉切换与断电恢复待验，不改变零拷贝路径 |

### G01 的代码证据与风险边界

此项不是仅凭关键词作出的推测：

1. `app/lib/server_settings.dart:174` 明确说明查看器需要 server-key 做主机解析和打洞证明；`app/lib/tunnel_agent.dart:195` 将它通过 stdin 交给代理。
2. `server/internal/api/api.go:644` 的 lookup 使用 `authorized()`；`:1526` 只比较部署级 server-key（未配置时开放服务查询）。
3. `server/internal/api/console.go:35` 的 `consoleActor()` 首先接受同一 server-key，并直接授予最高管理能力，不要求普通账号角色。
4. `server/internal/api/accounts.go` 的账号管理也走这套后台授权。

因此，**当管理员把该密钥发给普通连接者时，实际上也发出了管理能力**。这并不意味着匿名用户已经能直接控制任意桌面：主机端仍有自己的密码/令牌、审批、2FA 和权限检查。建议修复的是服务端信任边界及凭据用途，而不是否定现有主机认证。

**2026-11-09 状态：已实现。** `chast-server` 新增 `--link-key` 与 `--host-key`（要求同时设置 `--server-key`，三者互不相同；未设置角色密钥时保持旧单密钥语义）。API 层用 `linkAuthorized`/`hostAuthorized`/`serverKeyAuthorized` 分别守护 lookup/lookupBatch、注册/主机签名审计、管理入口（console/groups/wake/metrics/审计查询/录制管理），Bearer 比较为常量时间；UDP punch 与 TCP punch 的证明校验接受 link key 或管理 key，拒绝 host key 与错误 key。查看器 `ServerSettings.linkKey`（`connectionKey` 在为空时回退 server key）贯穿单条/批量 lookup、`chast-tunnel viewer --link-key-stdin` 与打洞；主机 `host_key` 经 ini、服务模式合并、`chast-tunnel host --host-key`/`--host-key-stdin` 覆盖注册、保活续期与审计转发，托盘连接服务对话框可编辑。验证：`go test ./...`（服务端全量）与 Flutter `analyze` 无问题、684 项测试通过；`chast-host-settings-test`、`chast-host-tunnel-test` 通过。角色密钥测试覆盖 lookup/批量 lookup、注册、主机审计、建号、身份重置与标签、管理录制列表、WOL、组、metrics、console 审计查询，以及真实 UDP/TCP punch 配对。真实 `chast-server` 回环验收：host key 注册、保活与签名审计（审计行写入成功、spool 消费后删除），link key lookup 并完成 UDP punch→TCP 直连，host key 被 viewer lookup 拒绝，link key 对注册/审计/metrics/groups 均返回 401，管理 key 可 lookup/metrics/审计查询。证据边界：回环不等于公网或真实代理路径；旧 `--server-key` 单密钥部署仍兼容，未做强制迁移。

## 四、环境验收与发布工程

### 有代码但仍缺真实环境证据

| 环境组 | 必须验证的工作流 | 现有依据 / 不能推出的结论 |
| --- | --- | --- |
| E01 网络 | 双机跨 NAT、对称 NAT、中继回退、代理、断网切网、v6-only/双栈 | 已有协议/回环测试；不能推出公网成功率、重连时延和吞吐 |
| E02 桌面输入 | 多显示器、负坐标、不同 DPI、AltGr/IME、真实笔和精密触控板、跨 GPU | 单机/native/组件测试存在；不能代替真实桌面交互 |
| E03 媒体 | HEVC/AV1 实际编码解码、摄像头、30 分钟双机听感、音频热插拔与 AEC | 硬件不支持后的回退结果只证明回退；无真实摄像头不等于媒体路径验收 |
| E04 主机与驱动 | 虚拟屏安装、无头启动、拓扑恢复守卫、服务崩溃/退出、WTS 切换 | 驱动签名/包预检和拒绝路径已做；实际拓扑和恢复仍待专用设备 |
| E05 身份和部署 | FIDO2/Windows Hello、实际 OIDC、企业 CA、ACME 续期、CDN/反代 WS | 模拟认证器与本机回环不能替代真实交互与公网证书生命周期 |

详细环境任务可复用 `docs/rustdesk-gap-tasks-2026-11-01.md` 的 ENV-01～12；此次未把它们重新标为代码缺失。

### 发布和运维工程项

| ID | 发现 | 建议 / 对照边界 |
| --- | --- | --- |
| O01 | 版本兼容严格相等：`src/net/protocol.cc:621` 拒绝任何非 v49 Hello | 规定最小兼容窗口、capability 协商和混合版本升级流程；RustDesk 有按对端版本降级分支，但不能据此宣称所有历史版本互通 |
| O02 | `.github/workflows/security.yml` 仅 Linux Go/Flutter 检查，注释明确 C++ 依赖本地发布流程 | 增加 Windows 原生构建和可模拟 CTest；GPU/驱动测试分配专机。此次不重复执行已经通过的本地测试 |
| O03 | 未找到受版本控制的 Dockerfile/Compose/systemd 模板；已有 Go 可部署程序 | 提供可复现镜像、部署模板、升级/恢复演练。RustDesk 官方已有 Docker/脚本路径（D02/D14），不是语言可编译即可视作部署体验相同 |
| O04 | PostgreSQL 存在，但 `Registry.hosts`、中继租约和 wake 队列仍是进程状态 | 不把数据库支持宣传成无状态多活；先说明单活/粘性/故障重注册模型，再做共享状态或故障转移。未取得 RustDesk Pro 任意 active-active 的实测，不作比较承诺 |
| O05 | 已有更新清单/发布者签名和回滚，打包脚本未形成完整自动化发布门禁 | 区分协议签名、更新包验证、EXE Authenticode、驱动签名四件事；发布流水线应产出版本、摘要、签名和验证记录。没有检查当前产物签名，不断言产物未签名 |
| O06 | 历史文档存在范围和时效混淆 | 维护此表的能力状态与环境证据，避免用“函数存在”“有 API”“回环通过”替代端到端完成 |

## 五、需要修正的旧对照结论

1. **“服务端无缺失项”范围过大。** 最多描述当时的会合/中继基础，不能涵盖 Pro 的连接 ACL、Control Role、Strategy、细粒度 Admin Role 和 API Token。
2. **“按组授权”不等于“按组决定连接资格”。** 本项目的 `AccountGroup` 注释明确指向控制台权限；地址簿共享也是信息访问授权，主机认证是另一层。
3. **默认键盘 Map 已有充分依据。** 1.5.1 `src/client.rs:3725` 会优先选受支持的 Map；Translate/Legacy 是剩余功能选择，Auto 并非本次确认的桌面菜单项。
4. **“AV1 已实现”和“录制已实现”不能合并成“AV1 可录制”。** 当前组合明确不成立。
5. **RustDesk 并非一律重编码后录制。** 1.5.1 `src/client.rs:2729` 向 recorder 传视频 frame，`libs/scrap/src/common/record.rs:194` 按压缩码流写入。`tools/mp4_recorder.h` 开头和旧文档把它概括为“重新编码解码帧”的表述已不适合作为当前对照依据。
6. **传输不能只写“RustDesk=WebRTC/ICE/TURN”。** 此次客户端基线确有 WebRTC 支持，但 OSS hbbs/hbbr 同时有 TCP/UDP/WS 相关路径；应按版本和具体链路说明。CHAST 的 QUIC 选型是架构差异，不自动判定为功能缺失或优势。
7. **虚拟显示器、多窗口、共享采集不是空白项。** 当前代码已有实现；应记录 provider/驱动/硬件验证边界。
8. **“便携提权不实施”是取舍，不是功能等价或技术上绝对不可能。** 安装服务解决长期无人值守，临时客户支持仍有独立使用需求；提权交接可单独设计，不应未经方案验证就认定必然破坏 GPU 路径。
9. **Pro 文档也有边界。** LDAP 文档写明未支持组同步，Web/iOS 不具备完整被控端能力，Strategy 同步后允许用户修改部分设置；本次不把这些误算成 RustDesk 已全面支持。

## 六、建议推进次序

建议第一批做 **G01 凭据隔离、G05 单向权限、G06 AV1 录制、G07 大目录分页**。它们边界清晰，能直接改善已有产品的安全性和可用性。

第二批完成 **G02/G03/G04 企业授权与策略**，以及 **G08/G09 剪贴板工作流、G10 字符输入、G11 自动录制**。前一组需要统一身份/票据设计，后一组需要端到端交互测试。

第三批按实际客户场景选择 LDAP、区域中继、MSI/定制分发、浏览器/移动端和打印。保留 GPU 路径与平台范围的现有取舍，先明确目标再立项，不在本次审查中擅自改运行代码。

## 七、官方来源和复查方式

本机审查材料保存于忽略目录 `build/review/full-parity-2026-11-07/`；包括两个官方源码压缩包及展开目录、官方文档树和 28 份通过 GitHub blob API 获取的文档。原始页面抓取不稳定时使用官方 API，未以社区文章替代官方证据。

官方仓库与发布查询地址：

```text
https://api.github.com/repos/rustdesk/rustdesk/releases/latest
https://api.github.com/repos/rustdesk/rustdesk-server/releases/latest
https://api.github.com/repos/rustdesk/rustdesk-server-pro/releases/latest
https://github.com/rustdesk/rustdesk/tree/23e5bb471a040d62f0a6ad52591fb30784265b73
https://github.com/rustdesk/rustdesk-server/tree/73523b31cfd25d77dee862e6fc9f5e1fb5e485ef
https://github.com/rustdesk/doc.rustdesk.com/tree/56f9310ca2c91f8e94b6a460290d1e94f62e0482
```

官方文档路径（相对于固定的 `doc.rustdesk.com` 提交）：

| 引用 | 路径 |
| --- | --- |
| D01 | `content/client/_index.en.md` |
| D02 | `content/self-host/rustdesk-server-pro/_index.en.md` |
| D03 | `content/self-host/rustdesk-server-pro/permissions/_index.en.md` |
| D04 | `content/self-host/rustdesk-server-pro/control-role/_index.en.md` |
| D05 | `content/self-host/rustdesk-server-pro/strategy/_index.en.md` |
| D06 | `content/self-host/rustdesk-server-pro/admin-role/_index.en.md` |
| D07 | `content/self-host/rustdesk-server-pro/ldap/_index.en.md` |
| D08 | `content/self-host/rustdesk-server-pro/address-book/_index.en.md` |
| D09 | `content/self-host/rustdesk-server-pro/relay/_index.en.md` |
| D10 | `content/self-host/rustdesk-server-pro/console/_index.en.md` |
| D11 | `content/self-host/client-configuration/advanced-settings/_index.en.md` |
| D12 | `content/client/windows/windows-portable-elevation/_index.en.md` |
| D13 | `v3/src/content/post/en/web-client-v2-preview.mdx` |
| D14 | `content/self-host/client-deployment/_index.en.md` |

本次仅新增对照报告和旧任务清单的索引提示。没有修改产品代码、服务器配置或数据库，没有读取真实剪贴板，也没有进行会话控制、提权、重启或部署操作。
