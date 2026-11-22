# chast 与 RustDesk 功能对照（2026-11-15）

本文件是 2026-11-15 的功能差异入口，用本机 RustDesk 1.5.1 源码重新核对
发射端、查看端、服务端和产品面。RustDesk 依据与本轮产品盘点日期固定在
2026-11-15；当前 chast 实现状态更新至 2026-11-22，协议基线为 **v78**。历史矩阵中与本文
冲突的行以本文为准：[全产品对照](rustdesk-full-parity-2026-11-07.md) 保留
C01–C51/S01–S27 的审查快照，[发射端模块对比](host-rustdesk-module-comparison-2026-11-08.md)
保留宿主逐模块记录。

## 1. 基线与判定口径

| 项目 | 本轮口径 |
| --- | --- |
| 我方 | `D:/ds-chcast`，分支 `codex/complete-workspace-2026-11-20` 的当前工作区，协议 **v78** |
| RustDesk | 本机 `D:/rustdesk`，HEAD `55786aa`（2026-11-03），`Cargo.toml` 版本 **1.5.1**；浅克隆只有 1 个提交，不能用提交区间推断引入时间 |
| 旧参照 | 多份旧文档固定 `7423dce`；`55786aa` 已补 Windows HDR 归一化、`static_refresh.rs`、QoS 重写和音频管道，旧文档中“参照没有 HDR/静止刷新”等判断已过期 |
| 方法 | 两侧源码阅读；RustDesk 侧只取行为与字段语义，不引入其代码或依赖；我方证据来自既有构建/测试记录，本轮未重跑完整原生 ctest |
| 证据等级 | **已具备**＝源码接入产品面；**自动化通过**＝有单元/回环测试；**真实链路已测**＝真实 QUIC/主机回环；**待人工**＝需双机、真实桌面或硬件；**缺失/部分/取舍/暂缓** 表示差异性质 |

本轮没有构建或运行 RustDesk 二进制，也没有做跨产品同条件的画质、延迟、弱网
或 NAT 成功率对测。因此本文只能说功能面与代码路径，不能说哪一侧“体验更好”。

## 2. 结论

1. 本表是功能差异盘点，不替代发布阻断审核。2026-11-22 已复现 C03：关闭一个
   标签后 Flutter 查看端进程退出，退出来源未定位；按约束关闭路径未改，当前
   生命周期/失败回滚验收未通过。Windows 主机主链路的其它通过范围仍以当前
   状态文档及对应原始验收为准。
2. 发射端在上一个审计基线 `55786aa` 之后的差距主要是**平台覆盖、分发工程
   和验收环境**，不是 Windows 单机主链路功能缺失。
3. **v76 自由手写标注关闭了旧文档反复出现的“白板自由标注未实现”**。需要
   更正概念：RustDesk 的 `src/whiteboard` 只有 `Cursor/Clear/Exit`，是把查看端
   光标、姓名和点击波纹画到主机屏幕的**光标覆盖层**，不是自由笔画；我方的
   `Annotation`（89）才是真正的临时自由手写，RustDesk 1.5.1 没有对应能力。
   因此这一项应从“缺口”改为“我方超出基线”，但真实双机等边界仍待验收。
   v77 进一步把 `Undo` 收紧为携带目标 `stroke_id`，多个查看窗口交错绘制时各自
   只撤销自己的笔画，不再误删其他窗口的最新笔迹。
4. X01、X02、X04–X06、X08、X10、X16 此后已在当前工作区实现，证据状态分别更新在
   下表；仍缺“跨本机多显示器铺满单画面”、查看端本地仅查看和忽略局域网发现等能力。
   X16 可配置快捷键已于 2026-11-22 完成并通过 Windows Release loopback；触控板、SVG
   应用互通和标签关闭仍有真实设备/运行验收边界。
   会话结束备注已于 2026-11-15 落地（见 X13、X19）。
5. 已决策暂缓/不做：远程打印、Switch Sides、AV1 4:4:4（及软件 AV1/VP8），
   理由见 [G19 决策](g19-decision-2026-11-14.md)；不应把它们混入“待修 bug”。
6. 真实双机、跨网 NAT、真实 UAC/锁屏、物理摄像头/麦克风、Explorer/Office
   粘贴、多 GPU/混合 DPI 等仍未验收，本文不把单机回环当作这些环境的证据。

## 3. 发射端（Windows 被控主机）

| 能力 | RustDesk 1.5.1 依据 | chast 现状 | 等级 |
| --- | --- | --- | --- |
| 采集 | DXGI + GDI 回退；HDR 归一化（`libs/scrap/src/dxgi/hdr.rs`） | DXGI → GDI 自动回退，FP16 归一化到 BGRA8/NV12 | 已具备，真实驱动/锁屏待人工 |
| 编码 | VRAM/HWRAM → libvpx VP8/VP9 → libaom AV1；VP9/AV1 4:4:4 | oneVPL → FFmpeg 厂商编码 → 软件 VP9；VP9 4:4:4；AV1 仅硬件 | 部分：缺 VP8、软件 AV1、AV1 4:4:4（v61 已关闭 VP9 4:4:4） |
| QoS/背压 | 每连接无界通道、全局码率控制、`static_refresh.rs` | 每订阅 60 帧/8 MiB 有界队列；全局 QoS 阈值/阶梯/冷却与 55786aa 对齐 | 自动化通过，真实弱网体感待人工 |
| 光标 | 33 ms cursor/pos 服务；形状按内容 ID 缓存，`request_cursor_data` 重取 | 独立 33 ms 采样线程、形状缓存、v73 `CursorShapeRequest` 重取 | 自动化通过；真实双机/形状驱逐待人工 |
| 协作光标/白板 | `src/whiteboard/mod.rs` 的 Cursor/Clear/Exit，主机透明覆盖层 | v68/v71 `ShowCursor` + 颜色/姓名/点击波纹；v76 再补自由手写 `Annotation` | 自动化通过 + 单机 QUIC 回环；真实双机/多屏 DPI 待人工 |
| 系统音频/语音 | `audio_service`，Opus LowDelay 10 ms，无 DATAGRAM/AEC | WASAPI loopback 扇出、Opus 10 ms、QUIC DATAGRAM、SpeexDSP AEC/NS、2 s 静音门 | 自动化通过，真实听感/双讲待人工 |
| 截图 | `ScreenshotRequest`；VRAM 路径会临时切换编码 | v70/v72 专用 CSHT 流：原始 BGRA→PNG 分片，不打断视频 | 真实主机回环已测；旋转/多屏/HDR 待人工 |
| 录制 | 压缩包直写 MP4；录制时 240 帧 GOP；`record_upload.rs` 录后上传 | 压缩包直写 MP4、10 分钟分段、无限 P 帧按需 IDR；入站录制只写本地，查看端录制可云端归档 | 部分：缺主机入站录制集中归档（差异，不是缺陷） |
| 多屏 | 每屏一个 `VideoService`；`follow_current_display` | 每屏独立 Source + 每连接订阅/选屏；合并画布；显示模式枚举/设置/恢复 | 自动化通过；多 GPU/旋转/热插拔待人工 |
| 隐私 | MAG、exclude-from-capture、虚拟显示三种后端 | 覆盖层 + `WDA_EXCLUDEFROMCAPTURE` + usbmmidd 虚拟屏；`vdd` 明确 Unsupported | 部分：驱动后端更窄，发布包/驱动验收是边界 |
| 虚拟显示 | IDD/Amyuni 后端 | usbmmidd 集成、安装预检、显示守卫 | 部分；PLAT-01 受驱动条款/签名阻塞 |
| 文件/剪贴板 | clipboard service + cliprdr；延迟文件读取；`FileRemoveDir` | CCLB v4（文本/HTML/RTF/DIB/PNG/文件列表）、方向策略、OLE 虚拟源、分页、隐藏项、多共享根、CFIL v2 压缩、递归删除、冲突协商 | 自动化/真实 QUIC 回环；Explorer/Office/跨网待人工 |
| 终端/端口转发/摄像头 | PTY、`port_forward_mux`、摄像头服务 | ConPTY、持久终端、最多 8 目标/32 连接、摄像头面板 | 已具备，真实会话待人工 |
| 主机扩展 | 重启/锁定/SAS、会话选择、远程配置、受信设备、TOTP | 全部具备；另有多查看者共享编码、会话权限票据/撤权 | 自动化通过；安全桌面/多用户待人工 |

## 4. 查看端（Windows Flutter）

已对齐：多显示器独立窗口与选屏、显示跟随、适配/原始比例、画质预设与自定义
百分比、自定义 FPS、编码选择（含 VP9 4:4:4）、相对鼠标/左右键互换/滚轮反转、
多触点/压感/Pan-Zoom、键盘 Map/Legacy/本地 IME、聊天、文件浏览与传输、终端、
摄像头、语音、录制与自动录制、统计、全屏、截图、标注、按设备偏好、显示器
窗口布局恢复、查看端录制上传、账号地址簿/分组/回收站/备注。

### 4.1 本轮新登记的查看端差异

| ID | 能力 | RustDesk 依据 | chast 现状 | 等级/优先级 |
| --- | --- | --- | --- | --- |
| X01 | 出站会话期间保持本机唤醒 | `kOptionKeepAwakeDuringOutgoingSessions`（`flutter/lib/consts.dart:232`） | Windows 查看端共享进程级 PowerRequest，双 Flutter Release loopback 验证首连接结束保留兄弟租约、末连接结束后归零；真实 OS 电源行为待验收 | 已实现；单机通过、实机待验 |
| X02 | 多连接标签页 | `kOptionOpenNewConnInTabs`（`consts.dart:89`、`desktop_setting_page.dart:551`） | 同一容器托管独立 Flutter engine；切换、输入释放/后台拒绝、媒体隔离、子窗口归属和父连接结束已有 loopback 证据；关闭单个标签会导致进程退出，来源未定位 | 部分；关闭生命周期为阻断项 |
| X03 | 用本机全部显示器铺满远控画面 | `use_all_my_displays_for_the_remote_session`（`consts.dart:259`、`src/client.rs:3127`） | 支持逐远端显示独立窗口/全部远端显示；无“跨本机多显示器拼接单画面” | 缺失，P3 |
| X04 | 自定义视图缩放百分比 | 工具栏 `Scale custom`（`remote_toolbar.dart:828`） | 按设备保存自适应/原始大小基准及 5–1000% 缩放 | 已实现；真实 DPI/全屏场景待验 |
| X05 | 光标缩放倍率 | 工具栏 `Zoom cursor`（`remote_toolbar.dart:1040`） | 独立光标倍率按设备保存，支持 25–400% | 已实现；真实 DPI/全屏场景待验 |
| X06 | 把剪贴板作为按键发送 | 工具栏 `Send clipboard keystrokes`（`remote_toolbar.dart:506`） | 用户显式读取本地纯文本，预览/编辑并确认后调用现有 Unicode 键入及两端权限校验 | 已实现；真实 Windows 剪贴板/输入法待验 |
| X07 | 每显示器自定义分辨率列表 | `PeerConfig::custom_resolutions`（`config.rs:383`）与重连重放 `try_change_init_resolution`（`ui_session_interface.rs:1589`），但条目只由虚拟屏切换路径写入（`handle_peer_switch_display`→`set_custom_resolution`，`ui_session_interface.rs:1546`），桌面端自由宽高输入也只对 RustDesk 虚拟屏出现（`isVirtualDisplayResolution`，`remote_toolbar.dart:2277`） | 物理输出两侧都只列主机枚举模式（我方 v78 `ListDisplayModes`/`SetDisplayMode`）；我方虚拟屏已接受 WxH 输入但会吸附到 `usbmmidd_default_modes()` 的 15 个固定尺寸（`virtual_display.cc:1456`），任意宽高随 PLAT-01 驱动解阻 | 部分；自定义尺寸依赖 PLAT-01（P3） |
| X08 | 触控板速度、边缘滚动、滚动样式 | `trackpad-speed`（`flutter/lib/models/input_model.dart:446`）、`kOptionScrollStyle`/`kOptionEdgeScrollEdgeThickness`（`consts.dart:86-87`） | 平移速度倍率、滚动条/自动/边缘模式、感应距离和坐标重映射均已实现并有 Flutter 回归 | 已实现；触摸/触控板、混合 DPI 手感待实机验收 |
| X09 | 查看端本地“仅查看” | `view_only`（`src/client.rs:3289`、`io_loop.rs:518`） | 有主机权限和工具栏输入开关；无查看端本地仅查看偏好 | 缺失，P3 |
| X10 | SVG 剪贴板格式 | `ImageSvg = 23`（`libs/base/protos/message.proto:344`） | CCLB v5 / 协议 v78；`image/svg+xml` 原生读写、活动内容拒绝、16 MiB 上限、QUIC 双向往返和坏 SVG 下文本/PNG 回退均有本机证据 | 已实现；实际应用互通待验 |
| X11 | 忽略局域网发现 | `Deny LAN discovery`（`desktop_setting_page.dart:1458`） | 主机 `--lan-discovery` 默认开启；查看端无忽略开关 | 缺失，P3 |
| X12 | 采集后端与低延迟模式开关 | `DirectX capture`（`desktop_setting_page.dart:614`）、`Optimize reaction time`（`remote_toolbar.dart:859`） | DXGI/GDI 自动回退、QoS 自动；无用户开关 | 取舍，P3 |
| X13 | 会话结束备注 | 断开时弹窗，向审计服务 `PUT /api/audit` 补写 `{guid, note}`，前提是本地选项 `allow-ask-for-note`、本会话已被服务器审计、审计 GUID 存在且尚无备注（`dialog.dart:1602`–`1720`） | 2026-11-15 已补：查看端侧栏「会话备注」开关（共享偏好 `view_ask_session_note`，**默认关闭**）→ 关闭会话时弹窗，备注随同一次 `POST /v1/account/connections` 落库（`session_page.dart:561`、`573`）；服务端 `connection_events.note` 列（schema v19，`migrate.go`），按 audit 引用只写一次，主机回执合并不覆盖（`connection.go:310`）；管理台连接审计显示并可按备注包含匹配过滤（`app.js:3021`、`index.html:496`），CSV 导出含 `note` 列 | 已具备（自动化通过；真实主机待人工） |
| X14 | 插入锁屏热键（Win+L） | 工具栏 `Insert Lock`（`remote_toolbar.dart:671`）→ `sessionLockScreen` → 主机 `lock_screen()`；两侧都是直接锁屏，**不注入 Win+L** | 主机动作菜单 `ViewerHostAction.lockScreen` → `LockWorkStation()`，受主机“锁定屏幕”权限约束 | 已具备（入口在主机动作菜单） |
| X15 | 共享地址簿密码 | `shared_password`（`src/client.rs:2883`、`4711`） | 密码仅本机 DPAPI；S20 端到端设计已写，未获产品确认，未改协议/DB/UI | 缺失，P2（待决策） |
| X16 | 可配置快捷键 | `shortcut_model.dart`、`src/keyboard/shortcuts.rs` | 支持切换远控、全屏、相对鼠标、统计面板和结束会话；可录制/清除，具有原生 hook 与 Flutter 回退 | 已实现；Release loopback 通过，物理键盘/IME 待验 |
| X17 | 便携提权保存 OS 密码并自动输入 | `os-password`（`src/client.rs:3042`）、`input_os_password`（`4424`） | UAC 交接 + 手动 OS 登录输入；不保存凭据 | 部分/取舍，P3（安全边界） |
| X18 | 查看端自动更新 | `kOptionAllowAutoUpdate`（`consts.dart:174`、`desktop_setting_page.dart:607`） | 有签名更新包，但需用户触发，由 `chast-tunnel apply-update` 替换重启；主机侧有自动更新 | 部分，P3（工程项） |
| X19 | 初始剪贴板同步开关 | `sync_init_clipboard`（`config.rs:351`、`io_loop.rs:1811`），默认取 `UserDefaultConfig`，缺省关闭 | 2026-11-15 已补：剪贴板按钮长按 →「连接时同步本机剪贴板」，设备级偏好，**默认关闭**；开启后下次连接把本机当前剪贴板作为首个变更推送 | 已具备（自动化通过；真实主机待人工） |
| X20 | 强制 TCP / 连接迁移 | RustDesk 数据面为 KCP/WebRTC，无同名选项 | 我方 QUIC 连接不迁移；查看端仅在连接阶段依次尝试 QUIC→TCP→WS | 我方工程项，不作为 RustDesk 差异 |

### 4.2 说明

- X03、X09、X11 与 X16 仍需按产品范围实现或决定；其它已交付行的真实设备验收
  不应误报为能力缺失。X14 是此前误判：RustDesk 的
  `Insert Lock` 同样只是请求主机锁屏，我方已有等价主机动作。
- X07 复核：RustDesk 的自由宽高输入只对虚拟屏开放（`isVirtualDisplayResolution`），`custom_resolutions` 由虚拟屏切换路径写入并在重连时重放（`try_change_init_resolution`）。物理输出的分辨率菜单两侧一样只列主机枚举模式，因此我方不做“物理屏任意分辨率”，该能力随 PLAT-01 虚拟屏驱动一起验收。
- X13 已落地（2026-11-15）：RustDesk 用断开时的 `PUT /api/audit` 按 audit GUID 补写备注，我方没有独立的按行更新接口，而是把备注挂在会话结束时的那一次 `POST /v1/account/connections` 上——审计行本来就是查看端在这一刻创建的，所以“只写一次”自然成立：主机回执合并时 `mergeConnectionNote` 保留首个非空值（`connection.go:310`），同一 audit 引用的重复上报也不会覆盖。备注上限 240 字符，服务端 v19 迁移加 `connection_events.note`，管理台按包含匹配过滤（`LIKE ... ESCAPE` 同时兼容 SQLite 与 PostgreSQL），CSV 导出多一列。查看端开关是共享偏好且**默认关闭**，只有本次会话确实被账号服务器审计时才弹窗。
- X11 与 X19 涉及隐私预期：前者是查看端是否接受未认证的局域网广播，后者是
  是否把本机历史剪贴板推给远端。X19 已按“默认保护用户”落地：开关缺省关闭，
  RustDesk 侧的 `UserDefaultConfig` 缺省也是关闭。X11 仍未实现。
- X12 是有意取舍：我方把采集后端和低延迟交给自动回退与 QoS，避免用户选错后
  得到黑屏或高延迟；若要加入开关，需要同时定义失败回退。
- X17 的“保存管理员密码”是安全取舍，不应为了对齐而默认保存凭据。
- X18 不是“没有更新”，而是“没有自动下载/安装”；运营者签名校验和回滚已存在。

## 5. 服务端与企业能力

| 领域 | RustDesk 1.5.1 / Pro 1.8.7 | chast | 等级 |
| --- | --- | --- | --- |
| 会合/中继 | hbbs/hbbr，KCP/WebRTC，GeoLocation（Pro） | 自有注册/租约/签名、QUIC 打洞、TCP/WS 载体、中继池限额、区域优先 | 已具备，真实跨网待人工 |
| 账号/2FA/SSO | Pro：账号、TOTP、Email、OIDC、LDAP（无组同步） | 账号、TOTP、恢复码、Email/SMS、WebAuthn、OIDC+PKCE、LDAP bind | 自动化通过，生产部署待人工 |
| 准入/策略 | Pro Access Control、Control Role | 设备策略、账号权限上限、签名票据、撤权推送、应急访问 | 自动化通过，G02/G03 生产链路待人工 |
| 地址簿 | Pro 地址簿/共享 | 地址簿、分组、回收站、备注、用户/组/Everyone 共享、分页、按需剪贴板 | 自动化通过；共享密码 S20 待决策 |
| 管理台 | Pro 管理台/API Token | 管理台、角色、API Token（设备/录制/账号组范围）、审计、Prometheus | 自动化通过，Postgres 真库已测 |

## 6. 平台、分发与已决策项

| 项目 | RustDesk | chast | 性质 |
| --- | --- | --- | --- |
| Linux/macOS 主机与查看端 | 有，平台能力有差异 | 无（CMake 要求 MSVC/DXGI，查看端仅 Windows runner） | 平台缺口 |
| Android/iOS/Web 查看端 | 有（iOS 不能作为被控） | 无 | 平台缺口 |
| MSI / 定制客户端生成器 | `res/msi`、`load_custom_client` | 单文件 `chast-setup.exe`、打包参数；无 MSI/品牌生成台 | 分发缺口（C07） |
| 多语言 | 55 个语言文件 | 查看端/主机中英双语，管理台主要中文 | 部分 |
| 远程打印 | `src/server/printer_service.rs` | 无 | 暂缓（G19） |
| Switch Sides | `SwitchSidesRequest` | 无（两套程序分离） | 不实施（G19） |
| AV1 4:4:4 / 软件 AV1 / VP8 | libaom/libvpx 支持 | 无 | 暂缓（G19） |

## 7. 本轮文档修正

旧文档里“白板自由标注未实现”的判断已过期，且把 RustDesk 的 whiteboard 误当成
自由绘画。本轮修正：

- [全产品对照](rustdesk-full-parity-2026-11-07.md)：C30 初始剪贴板同步从“缺失
  选项”改为“默认同步、缺开关”；顶部加 2026-11-15 入口说明。
- [发射端模块对比](host-rustdesk-module-comparison-2026-11-08.md)：G21 行与
  “仍待人工验证或未立项”改为 v76 自由标注已完成源码/自动化/单机回环；补记
  RustDesk whiteboard 只是光标覆盖层。
- [当前状态与计划](status-and-plan-2026-11-09.md)：v68/v71 两处“白板绘制未实现”
  和 G21 行改为 v76 状态；追加本轮新登记的查看端 P3 入口。
- [缺口任务](rustdesk-gap-tasks-2026-11-01.md)：G21 小节的“RustDesk 式白板绘制
  仍未实现”改为自由标注已由 v76 关闭；保留光标覆盖层边界。
- [发射端审计](sender-audit-2026-11-12.md)：结论与第 5.1 节移除“白板自由标注”
  缺口，改为已关闭并注明基线差异。
- [指针复核](pointer-acceptance-2026-11-11.md)：第 41 条移除“自由绘制标注仍未
  实现”。
- [发射端效果对照](sender-effectiveness-rustdesk-2026-11-11.md) 与
  [发射端审核](sender-audit-rustdesk-2026-11-11.md)：顶部加 2026-11-15 更正说明，
  原文保留为历史快照。

## 8. 待环境验收

以下项目在代码/自动化之外仍需真实环境，不计入“功能差异”：

- 双机 Windows：真实 NAT/防火墙/弱网、打洞成功率、中继回退、音频双讲、
  剪贴板双向、文件跨网吞吐、摄像头/麦克风。
- 真实桌面：UAC/锁屏/安全桌面输入、SAS、多用户会话切换、锁屏截图/隐私模式。
- 多显示器：多 GPU、旋转、负坐标、混合 DPI、热插拔、覆盖层目视位置。
- 企业与部署：生产 `chast-server` + 真实代理/中继、PostgreSQL、反向代理、
  受管策略与票据整链、多账号审计汇聚。
- 虚拟显示/打印：驱动签名与分发前置未解决前不承诺可用。

## 9. 复现入口

RustDesk 侧固定 `D:/rustdesk`（`55786aa`），关键路径：

- 工具栏：`flutter/lib/desktop/widgets/remote_toolbar.dart`
- 桌面设置：`flutter/lib/desktop/pages/desktop_setting_page.dart`
- 选项常量：`flutter/lib/consts.dart`
- 对端配置：`libs/hbb_common/src/config.rs`
- 白板：`src/whiteboard/mod.rs`、`src/whiteboard/windows.rs`
- 隐私后端：`src/privacy_mode.rs`
- 消息格式：`libs/base/protos/message.proto`
- 客户端：`src/client.rs`、`src/client/io_loop.rs`

我方证据以各模块日期文档和 `docs/` 中的 CTest/Flutter 记录为准；本轮没有新增
运行证据，不把旧记录改写成“本轮已测”。
