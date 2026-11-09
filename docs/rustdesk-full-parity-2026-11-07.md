# RustDesk 全功能对照：客户端、被控主机与服务端（2026-11-07）

> 后续发射端深入复核见 [主机实现、效果与修复顺序](rustdesk-host-parity-2026-11-07.md)。
> 下表保留首次审查快照；第一批主机 F01–F04 已修复，最新证据见主机报告第七节。
> 多屏服务、QoS、按 ID 管理与编码协商仍待后续批次。
> 当工作范围限定发射端时，以该报告的 F01–F08 和分批顺序为准；下文仍保留全产品视角。

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
| C06 | 便携使用与会话中提权 | 便携启动提权、接受并提权、控制端请求提权（D12） | **部分 / 取舍**：有 `--portable`，没有会话内 Request Elevation；已安装 SYSTEM 服务不能替代临时支持场景 | `host/main.cc`、`host/platform.cc`、旧任务 DEV-27/DEV-08 |
| C07 | 企业安装包 / 定制客户端 | MSI、批量脚本、Pro 定制客户端生成（C `res/msi/`、D10/D14） | **部分**：有 Windows EXE 打包和安装参数；未找到 MSI、品牌/配置生成台、配置锁定模板 | `tools/package-windows.ps1`、`server/cmd/chast-package/` |
| C08 | 多会话、多窗口 | 多连接与显示窗口（C） | **部分 / 有限额**：独立 Flutter engine、最多 16 个窗口；主机默认 2、上限 8 个查看者，但多个桌面会话共享首个会话的同一条画面流，不能由开窗能力推导独立选屏 | `app/lib/session_window.dart`、`include/chast/host_settings.h:61`、`src/host/host_server.cc:66` |
| C09 | 角色互换 Switch Sides | 双端角色交接（C `src/server/connection.rs`） | **缺失 / 取舍**：两套程序分离，缺反向可达性检查、进程交接及一次性授权 | `app/windows/viewer/`、`host/`；旧任务 DEV-14 |
| C10 | 多语言、会话快捷操作 | 多种语言、键盘菜单、快捷操作（C/D11） | **部分**：查看器及主机中英双语，管理台主要为中文；已有常用快捷键，不能据此认定整个快捷键定制系统对齐 | `app/lib/l10n.dart:9`、`host/i18n*`、`server/internal/webadmin/assets/index.html:2` |

### 画面、显示器与输入

| ID | 能力 | RustDesk 基线 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| C11 | 多显示器、合并桌面、切屏 | 显示器选择与多屏查看（C） | **部分 / 待验**：单屏、合并采集有路径；跟随者不能独立选择或切换显示器，逐屏开窗尚不等同独立多屏；多 GPU、负坐标及高 DPI 待实测 | `src/capture/merged_capturer.cc`、`src/host/host_server.cc:2337`、`:2490` |
| C12 | 跟随光标 / 前台窗口 | 对应会话选项（D11） | **部分**：生产会话有处理逻辑；跟随会话未消费 SetDisplayFollow 请求，可能等待超时 | `include/chast/protocol.h` 的 `DisplayFollow*`、`src/host/host_server.cc:2473`、`:5202` |
| C13 | 远程分辨率、缩放和光标 | 分辨率、视图比例、光标缩放（C/D11） | **部分**：显示模式修改、适配/原始比例、原生光标已有；跟随会话的显示模式查询/修改返回 Busy；真机 DPI 待验 | `src/capture/display_mode.cc`、`src/host/host_server.cc:2502`、`app/windows/viewer/remote_cursor.*` |
| C14 | 自定义画质和帧率 | 预设及自定义画质/FPS（D11） | **部分**：UI 为预设画质和默认/15/30/60 FPS；协议可到 240，缺任意数值入口；跟随者的画质修改被拒绝，所有观看者共享生产流 | `app/lib/widgets/remote_toolbar.dart:1625`、`app/lib/stream_prefs.dart:26`、`src/host/host_server.cc:2531` |
| C15 | H.264 / HEVC / AV1 | 具备多种编解码能力（C `libs/base/protos/message.proto`） | **已具备 / 待验**：三种硬件路径和回退梯级；不能把硬件不可用时回退 H.264 当作 AV1 实机成功 | `src/codec/vpl_encoder.cc:255`、`src/codec/ffmpeg_encoder.cc`、`app/lib/viewer_bridge.dart:3148` |
| C16 | VP8 / VP9 / 4:4:4 | VP8、VP9、I444 按端点能力使用（C `libs/scrap/src/common/codec.rs`） | **缺失 / 取舍**：当前 NV12 路径无 I444，也无 VP8/VP9 桌面编码；彩色小字的完整色度不是现有 NV12 所能表达 | `include/chast/capture.h:17`、`include/chast/encoder.h` |
| C17 | 采集 / 编解码兼容性回退 | Windows 有 GDI 和软件编码等回退（C `src/server/video_service.rs:643`） | **部分 / 取舍**：正式桌面路径要求 GPU；DXGI/硬件不可用时无法提供同等覆盖。CLI 测量解码器可软件回退，不代表 GUI 支持 | `include/chast/capture.h:22`、`include/chast/decoder.h:7`、`src/codec/ffmpeg_decoder.cc:144` |
| C18 | HDR 桌面处理 | 平台相关采集/色彩处理（C） | **已有有限实现**：FP16 采集后归一到 BGRA8/NV12；不宣称 HDR10 端到端或与所有显示器等效 | `include/chast/capture.h:106`、`src/capture/hdr_pass.cc` |
| C19 | 隐私模式 / 阻止本地输入 | 隐私实现及输入控制（C） | **已具备 / 待验**：遮罩和虚拟屏方案、阻止输入、撤权/断线恢复；驱动拓扑仍待实机 | `src/win/privacy.cc`、`src/host/display_privacy.cc`、`tests/block_input_test.cc` |
| C20 | 虚拟显示器 / 无头使用 | Windows 虚拟显示相关能力（C） | **部分 / 待验**：usbmmidd 集成、安装预检、显示守卫已做；不是“完全没有”。泛化 `vdd` provider 仍返回未实现，驱动包/许可/真机验收是边界 | `src/host/virtual_display.cc`、`src/host/host_server.cc:3699`、`tools/fetch-usbmmidd.ps1` |
| C21 | 默认物理键、组合键、锁定键 | 支持的对端默认 Map；有原生键盘捕获（C `src/client.rs:3725`） | **已具备**：低级钩子、HID、AltGr 过滤、按键释放和锁定键同步；此前按默认模式对齐的方向正确 | `app/windows/viewer/keyboard_capture.cc`、`src/win/input.cc`、`tests/keyboard_test.cc` |
| C22 | Legacy / Translate 模式 | 桌面菜单提供 Map、Legacy、Translate；Translate 标注 beta（C `flutter/lib/desktop/widgets/remote_toolbar.dart:2615`） | **缺失**：无模式切换与字符转换路径；跨布局直接输入体验不完全一致。Auto 仅协议/内部出现，不单列产品缺口 | `app/windows/viewer/keyboard_capture.cc:41`、`include/chast/protocol.h` 的 `KeyEvent` |
| C23 | 中文 / IME / 发送文本 | 依模式和平台处理字符输入（C） | **部分**：有 Unicode“发送文本”对话框；这不是本地 IME 组合输入自动进入远程画面的 Translate 体验 | `app/lib/widgets/send_text_dialog.dart:10`、`src/win/input.cc` |
| C24 | 鼠标、相对模式、触摸/笔、触控板 | 相应输入能力，平台有差异（C） | **已具备 / 待验**：相对鼠标、左右键互换、滚轮反向、多触点、压感、Pan/Zoom；实际触控板、笔和双机布局仍待验 | `app/lib/remote_pointer.dart`、`src/win/input.cc`、协议 v43/v48 |
| C25 | 每设备会话偏好 | `PeerConfig::load(id)` 保存对端设置（C `src/client.rs:3043`） | **已具备**：`ui.json` 根键保留全局默认，新增 `peers.<id>` 覆盖画质/编码/FPS/全色、适配/统计/滚轮/左右键/键盘模式；设备键为服务器 host ID 或直连地址，独立显示窗口同样按该键加载。文件浏览器排序/目录与终端字号仍为全局，属本地面板状态 | `app/lib/prefs_store.dart:36`、`app/lib/stream_prefs.dart:45`、`app/lib/view_prefs.dart:66`、`app/lib/pages/session_page.dart:336` |

### 剪贴板、文件与录制

| ID | 能力 | RustDesk 基线 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| C26 | 文本、HTML、RTF、图片剪贴板 | 多格式同步（C `src/clipboard.rs`） | **已具备**：异步读写、失败重试、基线、撤权取消已修复；系统原生调用的取消边界仍存在 | `src/clipboard/clipboard_worker.cc`、`docs/clipboard-async-fix-2026-11-07.md` |
| C27 | 共享事件驱动剪贴板 | 全局监听器、多订阅者（C `src/clipboard.rs:889`） | **已具备 / 待验**：进程级单监听（消息窗口 + `AddClipboardFormatListener`、原子代次、订阅者按 id 注册，首订阅启动/末退订停止）；原生后端由 `WM_CLIPBOARDUPDATE` 代次驱动并保留 5 s 兜底，合成后端或监听不可用时仍 500 ms 轮询；多窗口/多主机复制的人工复核待做 | `include/chast/clipboard_listener.h`、`src/clipboard/clipboard_listener.cc`、`app/windows/viewer/viewer_engine.cc`、`src/host/host_server.cc`、`tests/clipboard_listener_test.cc` |
| C28 | 文件复制粘贴 | Windows cliprdr 提供延迟请求文件内容（C `libs/clipboard/src/windows/wf_cliprdr.c`） | **部分**：收到列表就启动暂存下载，完成后写本地 CF_HDROP；最多 256 文件，FileList 只表示普通文件，不能保留纯空目录 | `include/chast/clipboard.h:45`、`src/clipboard/clipboard.cc:495`、`app/windows/viewer/viewer_engine.cc:3349` |
| C29 | 单向剪贴板 / 单向文件 | 有阻止被控端→控制端的独立策略（D11） | **缺失**：Clipboard/FileTransfer 各只有总权限位，无法做到“可上传但不可下载”或“只允许向主机粘贴” | `include/chast/protocol.h:1016`、`include/chast/host_settings.h:70` |
| C30 | 初始剪贴板同步选择 | 可选择连接时同步控制端已有内容（D11 `sync-init-clipboard`） | **缺失选项 / 取舍**：当前首次有效快照只建基线，默认保护历史内容；可增加明确的单次或持久化选择 | `docs/clipboard-async-fix-2026-11-07.md` |
| C31 | 完整远程文件系统浏览 | Windows 盘符枚举、目录浏览、隐藏文件选项（C `libs/base/src/fs.rs:53`） | **部分 / 取舍**：只能访问主机接收/共享根目录；没有盘符选择。不能简单移除路径校验，应增加明确授权的根目录集合 | `include/chast/protocol.h:705`、`src/clipboard/file_transfer.cc` |
| C32 | 大目录浏览 | RustDesk 目录模型返回条目列表（C `libs/base/src/fs.rs`） | **明确缺口**：单目录超过 1024 项被截断，无分页游标和下一页请求；UI 提示截断无法取回其余项 | `include/chast/protocol.h:715`、`src/clipboard/file_transfer.cc:775`、`app/lib/widgets/file_browser_panel.dart:113` |
| C33 | 上传下载、文件夹、批量、取消、断点续传 | 常规文件管理工作流（C） | **已具备 / 有限额**：已有目录队列、批量选择、续传/取消；单文件协议上限 16 GiB，主机可设更小限制 | `include/chast/protocol.h:637`、`app/lib/widgets/file_browser_panel.dart`、`src/clipboard/file_transfer.cc` |
| C34 | 递归删除、文件压缩传输 | `FileRemoveDir`/递归文件读取、压缩块（C `libs/base/protos/message.proto:409`、`libs/base/src/fs.rs:1106`） | **部分**：创建/重命名/删除已做，但删除限文件或空目录；CFIL 无压缩标记及处理链路 | `include/chast/protocol.h:750`、`include/chast/file_transfer.h` |
| C35 | 文件拖入和资源管理器衔接 | 桌面文件交互（C） | **已有部分**：WM_DROPFILES、本地上传选择、完成后打开/定位；不宣称等价于全部 OLE 双向拖放 | `app/windows/viewer/viewer_bridge.cc:2672`、`app/lib/widgets/file_browser_panel.dart` |
| C36 | 手动录制、分段、归档 | 支持会话录制及多种码流（C） | **已具备**：H.264/HEVC 原码流 MP4、尺寸/编码切换分段、可选账号上传；上传和录制是两项独立能力 | `tools/mp4_recorder.h`、`app/lib/pages/session_page.dart:503`、`server/internal/api/recordings.go` |
| C37 | AV1 录制 | AV1→WebM 录制路径（C `libs/scrap/src/common/record.rs:168`） | **缺失，且与已有选项冲突**：可选 AV1，但录制器拒绝该 codec；录制中切到 AV1 也不能继续按既有方式录制 | `tools/mp4_recorder.h:52`、`app/lib/widgets/remote_toolbar.dart:1667` |
| C38 | 主机自动录制 / 查看器自动录制 | 入站自动录制、出站自动录制、隐藏停止按钮选项（C、D11） | **缺失**：仅查看器手动录制与自动上传，主机无录制器；不能依赖查看器点击保证每次会话留档 | `include/chast/host_settings.h:100`、`app/lib/server_settings.dart:183`、`tools/mp4_recorder.h` |

### 主机控制、协作与专用会话

| ID | 能力 | RustDesk 基线 | 本项目现状及缺口 | 本地依据 |
| --- | --- | --- | --- | --- |
| C39 | 系统声音、麦克风、语音通话 | 音频传输及通话协商（C） | **部分 / 待验**：Opus、QUIC DATAGRAM、AEC、呼叫状态已有；系统音频广播受生产会话的音频权限和发送成功与否影响；双机听感和热插拔未验 | `src/audio/`、`src/host/host_server.cc:3351`、`:3391`；主机报告 F04 |
| C40 | 聊天与截图 | 客户端会话功能（C） | **已具备**：聊天 UI/协议、截图路径及权限 | `app/lib/widgets/chat_panel.dart`、`src/win/png_writer.cc`、`include/chast/protocol.h` |
| C41 | 远程终端、持久终端 | 终端服务、重连、输出回放（C） | **已具备 / 有限额**：两标签，100 个持久服务、1 小时空闲回收、1 MiB 回放；持久化默认关闭 | `include/chast/protocol.h:1564`、`src/host/terminal_relay.cc`、`app/lib/widgets/terminal_tabs.dart` |
| C42 | TCP 隧道 / RDP 入口 | TCP tunneling 与 RDP（C） | **已具备**：最多 8 规则及目标白名单、mstsc 启动；这不是通用 VPN/UDP 转发，也不把后者计为 RustDesk 缺口 | `include/chast/protocol.h:1483`、`src/net/port_forward_client.cc`、`app/lib/rdp.dart` |
| C43 | 摄像头及独立会话类型 | 桌面、文件、终端、摄像头、隧道（C） | **已具备 / 待验**：独立模式无需桌面流；摄像头完整媒体链路仍需真实设备 | `include/chast/protocol.h:1061`、`src/host/camera_relay.cc`、`app/lib/widgets/camera_panel.dart` |
| C44 | 重启、锁屏、Ctrl+Alt+Del、断开后锁定 | 主机操作（C） | **部分**：生产会话有授权和结果，跟随者的 HostAction 被拒绝；SAS 有单独处理，断开后锁定按最后观看者退出执行 | `src/win/sas.cc`、`src/host/host_server.cc:2572`、`:2578`、`:2202` |
| C45 | OS 登录 / 自动登录 | 登录相关字段及自动登录逻辑（C `src/client.rs:3031`、`:4450`） | **部分**：用户手动提供密码，在登录/锁屏界面输入；没有完整账号选择、保存凭据和自动登录策略。不能把它整体标成自动登录完成 | `app/lib/pages/session_page.dart:2005`、`include/chast/protocol.h:1823` |
| C46 | Windows 会话选择 | WTS/RDP 会话选择（C） | **部分 / 待验**：服务模式生产会话可列出/切换；跟随者未消费列表/选择请求；需要多会话环境验证 | `host/service.cc`、`src/host/host_server.cc:5365`、`:8539`；主机报告 F02 |
| C47 | 远程打印 | 打印服务和驱动适配（C `src/server/printer_service.rs`） | **缺失 / 平台前置**：无打印协议/客户端/主机链路，仍需驱动分发及签名方案 | 旧任务 PLAT-02；当前协议和主机模块清单 |
| C48 | 活跃会话防休眠 | 入站/出站保持唤醒选项（D11） | **未找到实现**：代码未见相应设置或 Windows 电源请求；长时间观看且无输入时需确认睡眠策略影响 | 全树检索 `SetThreadExecutionState`、`PowerSetRequest`、`keep_awake`；`HostSettings`/查看器设置 |

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
| S16 | 细粒度后台角色 | Pro Admin Role，可按资源动作和范围授权（D06） | **部分**：readonly/operator/admin + 组范围；没有任意资源/动作权限组合及独立角色分配 | `server/internal/api/console.go:9`、`server/internal/account/groups.go:78` |
| S17 | 自动化 API Token / 委派 | Pro 可选 Device/Audit/User/Group 等权限的 Token（D10） | **缺失**：现有 server-key 或账号会话令牌，没有独立可撤销、可定 scope 的机器令牌管理 | `server/internal/api/api.go`、`server/internal/api/console.go`、`server/internal/account/store.go` |
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
| G02 | **P1，企业准入前必做** | **连接 ACL 未落到连接链路**：S13 的组管理不能限制谁连谁，账号封禁也不是主机本地密码失效 | 账号/组→设备策略，禁用设备/账号、移组、过期票据均可拒绝；直连和经服务器两条路径明确受管/非受管策略，不允许回退绕过 |
| G03 | P1 | **账号级会话权限角色缺失**：S14 只有主机本地授权 | 服务端签名、绑定用户/主机/有效期的权限凭证；主机独立验证并按明确规则与本地同意合成；覆盖所有敏感操作和撤权。不需要让中继解密桌面数据 |
| G04 | P1 | **集中策略缺失**：S15 的“配置下发”目前仅会合地址列表；会话内改单机设置不能代替设备管理 | 策略对象、分配范围、优先级、版本、同步确认和失败可见；用户设置与策略的冲突行为有明确定义。RustDesk Strategy 本身也不等于所有值永久强制锁定 |
| G05 | P1 | **缺单向剪贴板/文件权限**（C29） | 分离每个方向；主机在普通下载、剪贴板文件、上传、暂停恢复等所有入口执行，不能仅禁用 UI |
| G06 | P1 | **AV1 与录制不可组合**（C37） | 增加 AV1 合法封装，或先提供明确的编码协商/选项互斥；覆盖开始录制、录制中切 AV1、回切及分段文件可读验证，不改变零拷贝采集 |
| G07 | P1 | **大目录条目不可达**（C32） | 加分页/游标或流式列举；10,000 项目录能逐页访问且递归下载完整；测试目录在分页期间变化的行为 |
| G08 | P1 | **文件剪贴板体验不等价**（C28） | 先保留空目录、展示数量/大小限制；若追求 RustDesk 体验，再实现按粘贴/读取请求传内容，避免仅复制即全量下载；覆盖取消、断线、覆盖源文件 |
| G09 | P1 | **剪贴板仍按会话轮询**（C27；进程级监听已补齐） | 已实现进程级系统变更通知与订阅者管理：原生后端事件驱动、5 s 兜底，合成后端保持 500 ms；既有异步读写与基线规则不变。剩余：多窗口/多主机复制的真实 GUI 复核，不回环、不串会话、不额外阻塞线程 |
| G10 | P1 | **键盘缺 Translate / Legacy 选择**（C22/C23） | Map 保持默认；补 Translate 并验证非 US 布局、AltGr、死键、Unicode/IME 与快捷键混合。Legacy 是否需要可随目标旧系统决定；不能把 Auto 枚举当作额外菜单承诺 |
| G11 | P1 | **自动录制链路缺失**（C38） | 分别实现入站主机/出站查看器自动录制、存储容量/失败提示/保留策略；如要求集中留档，应由受管主机负责并报告失败，不能只看可选上传 |
| G12 | P1 | **多人管理权限颗粒度和自动化凭据不足**（S16/S17） | 资源/动作 scope、组范围、独立机器令牌的创建/到期/撤销/审计；控制台委派与普通连接权限分开 |
| G13 | P2（跨地区部署可升 P1） | **无就近中继**（S06） | 在现有健康/黏性基础上按区域或两端实测延迟选择；节点失效回退；用真实多地区网络验收收益 |
| G14 | P2（AD 用户可升 P1） | **LDAP 认证缺失**（S11） | LDAP/LDAPS 连接测试、过滤、登录、首次建号、账号停用语义；不把 RustDesk 文档也未支持的 LDAP 组同步当作必须对齐项 |
| G15 | P2 | **固定画质选项和显示器窗口布局**（C14/C25；按设备偏好已补齐） | 已按设备身份存储偏好并保留全局默认；剩余：自定义 FPS/质量数值入口及显示器窗口布局恢复 |
| G16 | P2 | **地址簿高级协作不全**（S20/S21） | 先补备注、回收站和组织组共享；云端密码同步另做密钥/撤销设计，不直接上传现有 DPAPI 密文或明文 |
| G17 | P2 | **临时支持流程不全**（C06/C09/C45） | 分别设计便携提权、角色交接、OS 登录便利功能；要求本地同意、内存凭据传递和可恢复的交接，不沿用“不装服务就绝对做不到”的假设 |
| G18 | P2 | **完整文件管理范围和操作**（C31/C34） | 可授权多个共享根/盘符、隐藏文件选项、递归删除确认、可协商压缩；保留根目录和重解析点防护 |
| G19 | P2 | **平台和兼容性覆盖**（C02–04/C16–17/C47） | 分开评估 Web、Linux 查看器、移动端、其他主机平台、打印、4:4:4及软件回退；均为独立项目，不能靠 Flutter 或 WS 标签认定完成 |
| G20 | P2 | **防休眠和界面入口一致性**（C10/C48/S24） | 明确入站/出站保持唤醒，退出恢复电源请求；管理台登录接已有认证后端，语言与错误状态一致 |

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
