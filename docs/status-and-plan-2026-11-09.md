# 当前状态与后续计划（2026-11-09，检查点 2）

本文件记录 `main` 分支在 v69 阶段的当前状态和下一步开发顺序，作为 [开发检查点](development-checkpoint-2026-11-09.md) 之后的补充入口。本文件随本次提交保存；本机 `build/` 日志、验收程序和临时配置不进入仓库。

2026-11-10 提交检查点：工作区累计的 v62–v65 变更按用户要求保存到本分支，包括票据/受管策略与准入、Legacy 键盘、查看端自动录制、机器令牌、递归删除和隐藏项浏览；当时协议 **v65**。v65 的协议/模糊/文件传输定向测试、`flutter analyze`、Windows 观看端 Release 构建和真实 QUIC 回环已通过；全量 Flutter 701/701 在按设备范围的隐藏项偏好修正前完成，修正后以定向 16/16 复验。完整 Release `ctest` 当时仍在本机后台运行，结果只记入本机日志，不回填本次提交。撤销推送与 server + 代理真实链路验收已完成。当前工作区在提交之后继续加入多共享根/盘符（协议仍 v65）与 CFIL v2 文件压缩（协议 **v66**）。

2026-11-10 键盘更新：协议 v63 新增 Legacy 键盘模式（查看端发送 Windows 虚拟键码，主机注入同名按键，由主机布局和输入法解释），查看端菜单/按设备偏好/主会话与显示器子窗口均已接入。自动化、构建与待人工验证边界见 [键盘 Legacy 模式](keyboard-legacy-mode-2026-11-10.md)。

2026-11-10 管理权限更新：账号 schema 升到 v11，控制台新增资源集合 × 角色动作的机器令牌（`chast_api_` 前缀、默认 90 天、每用户 20 个、即时撤销、owner 降级收窄、创建/撤销审计），组范围已接入账号组与组审计。Go 全量测试、vet 与 HTTP 回环集成测试通过；真实部署/Postgres 与设备组范围仍属边界，见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md)。同日后续补齐设备/录制组范围与管理台“API 令牌”页（本地真实浏览器冒烟通过）；真实部署、Postgres 与生产反向代理仍属边界。

2026-11-11 RustDesk 二次比对（固定提交 `7423dce`）：发射端常用主链路没有新的 P0/P1 缺口，新增三项 P3 差异——显示我的光标/白板（G21）、主机语音呼叫麦克风选择（G22）、会话期间移除壁纸（G23）；同时把截图从“已具备”改为“部分对齐”（观看端解码帧落盘 vs 主机 `ScreenshotRequest`）。直连 `host:port`、观看端麦克风选择、LAN 发现、WebSocket、IPv6 打洞、临时密码、空闲自动断开、双向语音和自动更新均已核实存在，不计缺口。详见 [发射端模块对比](host-rustdesk-module-comparison-2026-11-08.md)。

2026-11-11 宿主 P3 修正（G21/G22/G23）：协议 **v68** 新增 `ShowCursor`（83）控制消息，主机在独立线程创建分层、点击穿透、不激活的覆盖层绘制查看端指针，查看端工具栏开关按设备保存并按 33 ms 节流发送，主会话与独立显示器窗口共用同一偏好；RustDesk 式白板绘制未实现。主机语音呼叫新增 `voice_microphone` 设置、设置页/托盘入口，呼叫开始时读取，固定设备缺失时回退默认通信设备并记日志。新增默认关闭的 `remove_wallpaper`，授权会话开始备份并清屏、结束恢复，下次启动恢复异常退出遗留的备份。验证：`protocol`、`host_settings`、`host_voice` 通过（合成采集与注入壁纸移除器，未改真实桌面），`chast-host` Release 构建通过，`flutter analyze` 无问题，`display_menu`/`view_prefs` 定向 57/57。本机无物理麦克风，真实录音/设备切换、壁纸视觉切换、白板绘制和多屏覆盖层位置待人工验证；三项均不改变零拷贝媒体路径。

2026-11-11 检查点 v69（工作区）：协议 **v69**。文件同名冲突在两端都改为显式决策：目标已存在时主机发送 `FileConflict`（84，携带 identical 位、现有大小与修改时间），查看端弹窗选择跳过/覆盖/取消并可记住本队列的选择；新增 `FileConflictAnswer`（85）与 `FileCode::Skipped`（7）/`Conflict`（8），被跳过与失败分开计数，主机等待期间不设超时、也不创建文件。CCLB 升到 v3：每条目单独 zstd（≥256 字节才尝试，压缩不变小则存原样），条目头同时记录 stored/raw 长度，解压长度必须等于声明值。主机在基准建立后把当前剪贴板主动发送一次，查看端不再等到第一次变化才有内容。主机 QoS 探测与采样对齐 RustDesk `TestDelay`，改为 1 秒节拍（统计仍 500 ms），只有新回复且采样到期才推进控制器。独立 relay 节点新增 `POST /v1/leases/{id}/managed`，策略变更时立即翻转活动租约的受管标记（无活动租约由下次注册收敛）；代理传输凭证刷新余量 15→30 秒，直连/载体路径新增 20 秒凭据续签循环。移除未接线的 `BitrateController` 及测试；查看端精灵上限 512 px、仅查看远端指针叠层和独立显示器窗口“显示我的光标”同步落地，见 [指针四项复核](pointer-acceptance-2026-11-11.md)。验证：`protocol`、`file_transfer`、`remote_cursor`、`clipboard_async`、`viewer_probe --file-conflict` 与 `host_parity` 全量场景通过（`build/parity-full.log`：1012 个 ok、0 失败），Windows 观看端 Release 重建通过；`build/` 中的查看端带临时光标诊断，诊断源码已移除，干净重建后需一次冒烟。边界：文件冲突弹窗、剪贴板初始同步与逐项压缩尚无双机/真实剪贴板验收；真实 relay 节点、双机弱网与 G02/G03 真主机整链仍待环境。

2026-11-10 文件管理更新：协议 v64 新增 `FileOperation::RemoveTree`，观看端删除文件夹时确认后递归删除整棵树；主机先预检 reparse point 和只读条目，任一命中即整体拒绝，再按从深到浅顺序删除。协议/文件传输/文件面板测试、Windows 观看端 Release 构建和真实 QUIC 回环通过，Release 全量 ctest 69/69 通过；当时多共享根/盘符和压缩块仍属 G18，随后均已补齐，见 [文件夹递归删除](recursive-remove-2026-11-10.md)。

2026-11-10 隐藏项更新：协议 v65 让 `FileListRequest` 携带 flags 字节（bit0 = 显示隐藏项），主机在分页前过滤 `FILE_ATTRIBUTE_HIDDEN`，观看端文件面板新增显示开关并按设备持久化，递归下载沿用当前视图；两端必须同版本部署。协议/模糊/文件传输定向测试、全量 Flutter 701/701、Windows 观看端 Release 构建和真实 QUIC 回环（默认列表无隐藏项、显式视图出现）已通过，见 [隐藏文件浏览选项](hidden-files-2026-11-10.md)。

2026-11-10 多共享根更新：`host.ini` 新增 `file_share_roots=`（分号分隔、最多 16 个绝对目录，接收文件夹自动并入并去重），主机在文件传输会话顶层提供虚拟别名（盘符根 `C:\`→`C`，重名追加 ` (2)`），列表、下载、递归下载、上传和文件操作统一先拆别名再走原有沙箱；共享根不可被远程重命名或删除。协议保持 **v65**，没有新增线上字段。设置/文件传输定向测试、Windows 原生 Release 全量构建和真实 QUIC 回环 `--multi-shares`（3 个别名、嵌套列出、2/2 文件递归下载、上传落到共享子目录、未知别名与根重命名/删除拒绝）已通过，见 [多共享根与盘符](multi-share-roots-2026-11-10.md)。

2026-11-10 文件压缩更新（G18 收尾）：协议 **v66** 为 CFIL 增加可协商的 v2 块流，9 字节块头携带 `stored_size`/`raw_size`/压缩位；zstd level 1 按块压缩，压缩不减小则回退原始块，接收端跨 QUIC 缓冲重组并按块解压。仅对不小于 4 KiB、非剪贴板且非已压缩容器的文件启用；续传仍走 v1。协议/模糊/文件传输/设置定向测试、Windows 原生 Release 全量构建和真实 QUIC 回环通过：1 MiB 可压缩 `.txt` 的 CFIL 内容为 1776 字节，300 KiB 随机 `.bin` 为 307245 字节（5 个原始块多 45 字节块头）。完整 Release ctest 70/70 通过（1130.84 秒，`build/ctest-v66-full-2.log`）；首轮 69/70 的唯一失败是既有 `quic_merged` 视频解码计数抖动（“every received access unit decoded to exactly one picture”），该测试单独复跑与第二轮全量均通过。见 [文件传输压缩](file-compression-2026-11-10.md)。G18 的分页、递归删除、隐藏项、多共享根和压缩均已关闭。

2026-11-10 画质/帧率与窗口布局更新（G15 收尾）：协议 **v67** 让 `SetStreamOptions`/`StreamOptionsResult` 携带 `quality_percent`（自定义画质 10–100，预设必须为 0，结果回报主机实际生效值）。查看端新增自定义画质百分比（50 对应均衡的 1.0 码率比）与自定义 FPS（5–120）数值入口，菜单保留 15/30/60 快捷值，最后一次数值按设备保存；主机 `VideoQos` 以最新投票在预设/自定义之间切换目标码率比，自定义画质只保留 0.1 下限，相机中继与状态回报同步携带实际百分比。显示器子窗口按“设备 + 显示序号”把物理像素位置/尺寸写入 `%APPDATA%\chast\display-windows.tsv`，关闭保存、重开恢复，不与任何显示器相交的矩形被忽略。顺带修正主机身份证书库打开方式：先以 `CERT_STORE_OPEN_EXISTING_FLAG` 打开已有库，仅在库确实缺失时才允许 CryptoAPI 创建，避免部分配置文件拒绝“可创建”打开导致所有主机型测试失败。本机加固配置还会拒绝本地构建进程向既有 `%TEMP%` 目录和 `Downloads\chast` 新建文件：`security`、`host_security` 与 `chast-quic` 下载夹具改为进程私有或每次删除重建的临时子目录，`chast-viewer-probe` 在接收目录不可写时临时改名重建、退出时恢复原目录。协议/模糊/QoS/桌面流/`host_parity` 定向测试、`flutter analyze`、全量 Flutter 710/710、Windows 观看端 Release 构建与原生布局验收程序通过。全量 Release ctest 首轮 62/70，8 项失败均源于上述夹具限制；修复后 8 项复跑 7 项通过。第二轮全量 68/70，剩余两项为既有 `quic_weak_network` 自适应码率恢复失败和 `desktop_stream` 全队列饱和基线的取样抖动（后者同一二进制在首轮通过）。随后定位弱网恢复：真正的问题是恢复判据只读会话末的最终目标，控制器在恢复后又会因下一次探测合法回退，末次采样因此抹掉已经发生的回升；`quic_weak_network` 改为以“最低目标之后出现过更高目标通告”作为恢复证据，诊断行改为 configured/final/lowest/recovered，定向连续 4 次通过（最低 1000 kbps、回升 1749 kbps、30 fps，日志 `build/weak-abr-v67-run1..4.log`）。其间曾把静止刷新帧计入共享 QoS 发送计数并声称对齐 RustDesk `send_counter`；2026-11-11 复核 `static_refresh.rs` 后确认 `try_encode` 不增加 `send_counter`（固定基线 `7423dce` 无该模块），该改动及三份文档中的相关说法已撤销，刷新帧不计入动态画面证据。撤销后 `video_qos`/`desktop_stream`/`quic_weak_network` 定向 3/3，弱网直跑 2 次通过（496/492 个访问单元，最低 1000 kbps、回升 1749 kbps、30 fps，`build/weak-abr-v67-revert-run1..2.log`）。`desktop_stream` 在饱和断言前等待队列高水位稳定，干净构建连续 4 次通过（日志 `build/desktop-stream-v67-run1..3.log` 与本机另一次）。撤销后全量 Release ctest **70/70** 通过（917.55 秒，`build/ctest-v67-full-4.log`）。随后为 `VideoQos::tick` 的周期入口增加 1 秒节流（完整调价仍保持 3 秒冷却），避免高延迟未确认时每 33 ms 清零动态计数；新增确定性 `video_qos` 断言，去掉节流即失败，`video_qos` 110 项与 `desktop_stream`/`quic_weak_network` 定向 3/3 通过（`build/video-qos-cadence.log`、`build/ctest-v67-cadence-1.log`）。全量构建还修复了 `tests/protocol_test.cc` 中 `preset_percent` 的重复定义，`protocol` 测试通过；节奏修复后的全量 Release ctest **70/70** 通过（946.63 秒，`build/ctest-v67-full-5.log`；2 项剪贴板监听测试按既有条件跳过），完整 `go test ./...` 同轮通过。真实双机联动与多显示器人工目视仍待验收。

2026-11-10 G09 真实 GUI 复核与多窗口关闭修复：新 Release 查看端同进程连两台主机（62670/62671），两会话各 30 fps 且帧计数独立递增；先关闭首页窗口（首个 engine，进程级 Dart VM 所有者）后进程存活、两个会话继续串流（A 帧 5340→6378，B 帧 634→2422），无新 WER/转储；再关闭会话 A 后 B 独立串流；关闭最后一个会话后进程干净退出，仍无新 WER。剪贴板方面，首页关闭后一次本地变更恰好产生 2 条发送（每个存活窗口 1 条，叠加层 A/B 均为 发 1 收 0）；关闭 A 后再改一次只有 B 发送（发 2），发送行数与存活窗口数一一对应，无跨会话串发。修复两个不同的关闭崩溃：(1) 旧构建在 `WM_DESTROY` 中析构 engine 导致 `flutter_windows.dll` fail-fast 0xc0000409，改为 `~FlutterWindow` 析构；(2) 随后出现首个 engine 在其它窗口仍派发消息时被析构的 AV，转储 StackWalk64 显示 `dart::DartEntry::InvokeFunction+0x103 [dart_entry.cc:169]` 跳往未映射的 Dart AOT 地址，`main.cpp` 改为消息循环内不销毁首个窗口、循环结束后按子先于首个 engine 的顺序析构。观察：主机 B 首次连接一次失败（审计 `announce: capture failed: QUIC StreamOpen(video) failed: 0x80004004`），界面重连后成功，未根因。边界：单机共享同一系统剪贴板，主机回写被本机监听视为自回声，接收计数为 0；真实接收方向仍需两台机器。

2026-11-10 G04 集中策略更新：账号 schema 升到 v14（v13 新增 `policy_objects`/`policy_assignments`，v14 新增按设备单调分配的 `device_policy_versions` 序列）。服务端支持可复用策略对象、全部/分组/设备三种范围的分配、优先级与同优先级“设备 > 分组 > 全部”决胜，解析结果继续物化到唯一的 `device_policies` 准入路径；手动设备策略优先于所有分配，删除后回落到匹配分配。分配或对象变更会重算受影响的设备并经既有主机事件长轮询推送 `policy`/`managed=false`；设备生效版本跨回退、重建和删除单调递增，避免代理忽略低版本。管理台“设备策略”页拆为策略对象、策略分配、设备策略（生效结果）三块并显示来源；`go build ./...`、`go vet`、`go test ./...`、管理台 CSP 测试通过；真实 `chast-server` + `chast-tunnel host` 回环 26/26 覆盖注册下发、v2 设备分配推送、v3 回退、签名回执与解除受管。真实 `chast-host.exe` 受管会话整链、Postgres 实例与多操作者并发编辑仍属边界。

2026-11-10 H20 光标投递与全队列背压修复：对齐 RustDesk 独立光标服务，把光标采样从会话循环移到显示采集线程（`Source::notify_cursor` 在捕获后、编码前调用每订阅 sink，`CursorPublisher` 完成可见性/坐标/形状去重与 33 ms 节流），视频编码慢或发送队列满时不再推迟光标；指针专用唤醒只刷新元数据，不产生视频帧。同时修复全队列满载时的背压漏洞：所有订阅队列达到上限且采集返回 `NoNewFrame` 时，旧路径仍会把待处理的桌面变更、强制关键帧或静态刷新编码进注定被丢弃的窗口；现在直接返回并保留待处理状态，队列腾出后的第一个空闲 tick 用记住的图片 `encode(false, true)` 真正编码。验证：`desktop_stream` 连续 3 次通过（422 项断言，含全队列满载时变更保持待处理、腾出后编码）；`host_parity`（189.15 s）、`cursor`、`remote_cursor`、`multi_session`、`display_subscriptions`、`host_multi`、`video_qos` 通过；修复后全量 Release ctest 70/70（1148.07 s）；真实路径 8 s 静止桌面 `frames=236 encoded=234 pointer-only=0`、`cursor sent=1`，光标持续移动 8 s `frames=228 encoded=226 pointer-only=0`、`cursor sent=219 shapes=23 dropped=0 errors=0`；500 kbps / 30 ms netem 下 470 个访问单元全部送达/解码，最大帧年龄 4116 ms，光标 `sent=156 dropped=0`。真实双机高延迟/丢包的节拍、形状切换与坐标精度仍待验收。

2026-11-09 审核后修复：打包完整产品清单、剪贴板慢启动回退、显示器子窗口偏好、跨服务器偏好隔离及主机隧道凭据 stdin 传递已修改。Flutter 689 项、Go test/vet、独立原生 5 项及打包夹具通过；本机媒体依赖缺失，未重做完整原生构建。详见 [审核与修复记录](code-review-2026-11-09.md)。旧裸设备 ID 偏好保留但不自动迁移至服务器命名空间；G02/G03 仍待开发。

## 1. 基线

- 仓库：`ltpara/ds-chcast`；分支：`main`。
- 上一检查点提交：`0a23a93`（`chast: checkpoint v68 host P3, transport credentials and display prefs`）。
- 应用媒体/控制协议：**v69**；当前不承诺与 v68 及更早版本或 RustDesk 线协议互通。
- RustDesk 行为对照固定于本地参考提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`；只参考行为与字段语义，不引入其代码或依赖。
- 本次提交范围：工作区累计的 v67 收尾、文件同名冲突、剪贴板初始同步与 CCLB v3、QoS 1 秒节拍、relay 受管标记即时推送与凭据续签、512 px 光标与仅查看指针、未接线 `BitrateController` 删除，以及对应文档、原生测试、Go 测试和 Dart 测试。`build-review-clean/`、`server/chast-setup.exe`、`build/` 下的本机证据与构建产物不提交。

## 2. 已完成并进入生产路径

| 能力 | 状态 | 证据 | 边界 |
| --- | --- | --- | --- |
| v61 VP9 / 4:4:4 收尾（P0-1～P0-4） | 实现并完成本机验收 | 原生回归 8/8、Release 全量 ctest 64/64；真实 DXGI → VP9 → QUIC → GPU → 双 Flutter 窗口；1080p 六轮性能测量；打包 EXE 白名单 | 单机回环、单显示器、本机核显；真实 HDR、多显示器/竖屏、独立显卡、安装/升级/卸载流程未复验 |
| 静止桌面刷新对齐 RustDesk `static_refresh.rs` | 已接入 | `desktop_stream` 408–410 项、`host_parity` 976–977 项连续 3 次 0 失败；真实 Intel GPU 双查看器 92 项 0 失败 | 未做跨机画质/功耗对比 |
| 按设备会话偏好（RustDesk `PeerConfig`） | 已接入 | `stream_prefs` / `view_prefs` 26 项、会话窗口与显示流 8 项、`flutter analyze`、Release 构建 | 未做真实双主机人工切换复核 |
| 进程级剪贴板监听（G09 的 C27 部分） | 已接入 | 私有窗口站真实 `CF_UNICODETEXT` 测试 9 项、`clipboard_async`、`host_parity` 976 项 0 失败；真实 GUI 双会话（62670/62671）一次本地变更每个存活窗口恰好发送 1 条，关闭首页后按窗口独立，关闭一个会话后仅剩窗口发送，无跨会话串发 | 单机共享同一系统剪贴板，接收方向被自回声抑制（收 0）；服务模式/非交互窗口站与真实双机接收方向未验 |
| 会合服务角色密钥分离（G01） | 已接入 | Go `go test ./...` 全绿；Flutter 684 项通过；`chast-host-settings-test`、`chast-host-tunnel-test` 通过；真实 `chast-server` 回环完成三密钥正/负向验收 | 公网 NAT、真实反向代理、旧单密钥部署迁移、密钥分发与轮换未验证 |
| 单向剪贴板/文件（G05）、AV1 录制（G06）、大目录分页（G07） | 已完成 | 见 [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md) 与检查点文档 | 真实素材和跨网吞吐仍按各自模块边界待复验 |
| 键盘 Legacy 模式（G10，v63） | 已接入 | 协议往返/拒绝路径、主机虚拟键注入、修饰键与锁定键同步、查看端模式切换和按设备偏好自动化通过；`flutter analyze`、完整 `flutter test`、Windows 查看器 Release 构建、Release 全量 ctest 69/69 通过 | 真实双机非 US 布局、AltGr、死键、远端 IME、快捷键混合和安全桌面输入待人工验证 |
| 查看端出站自动录制（G11） | 已接入 | `view_prefs`/保留策略/工具栏菜单/会话自动开始自动化通过；全量 Flutter 698/698、`flutter analyze` 无问题；原生 Release 重建含容量守卫，Release 全量 ctest 69/69 通过 | 真实会话自动开始、512/64 MiB 阈值、10/30 档清理与长按菜单目视待人工验证；`hide-recording-button` 属受管策略未实现。详见 [查看端自动录制](viewer-auto-record-2026-11-10.md) |
| 管理权限颗粒度与 API Token（G12） | 已接入 | schema v11；`go test ./...` 全绿、`go vet` 无问题；存储层 4 项与 HTTP 回环集成 3 项覆盖资源拒绝、组范围 403、设备/录制按组过滤、全局面拒绝组范围令牌、角色上限、降级收窄、封禁、配额、到期、撤销和审计 | 设备/录制的组范围过滤与管理台“API 令牌”页已实现，并完成本地真实浏览器创建/撤销冒烟；真实部署升级与 Postgres 实例未验证。详见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md) |
| 文件夹递归删除（G18 部分，v64） | 已接入 | 协议/文件传输定向测试、文件面板 50 项、`flutter analyze`、Windows 观看端 Release 构建和真实 QUIC 回环（父/子文件夹创建→重命名→非空递归删除）通过；Release 全量 ctest 69/69 通过 | 只支持 Windows 主机；reparse point 与只读条目整体拒绝；预检后仍可能部分删除。多共享根/盘符与压缩块随后由 v65/v66 补齐。详见 [文件夹递归删除](recursive-remove-2026-11-10.md) |
| 隐藏文件浏览选项（G18 部分，v65） | 已接入 | 协议/模糊/文件传输定向测试、`flutter analyze`、全量 Flutter 701/701（含文件面板 51 项与视图偏好 16 项）、Windows 观看端 Release 构建和真实 QUIC 回环（默认列表无隐藏项、显式视图出现）通过 | 只识别 Windows `FILE_ATTRIBUTE_HIDDEN`；隐藏项只是显示过滤而非安全边界；多共享根/盘符与压缩块随后由 v65/v66 补齐。详见 [隐藏文件浏览选项](hidden-files-2026-11-10.md) |
| 多共享根与盘符（G18 部分，v65） | 已接入 | 设置/文件传输定向测试、Windows 原生 Release 全量构建、真实 QUIC 回环 `--multi-shares`（3 个排序别名、嵌套列表、2/2 文件递归下载、上传到共享子目录、未知别名与共享根重命名/删除拒绝）通过 | 仅 Windows 主机；共享根须在会话建立时可访问且非 reparse point；别名不参与分页。详见 [多共享根与盘符](multi-share-roots-2026-11-10.md) |
| CFIL v2 文件压缩（G18 收尾，v66） | 已接入 | 协议（块头/版本/选择规则）、模糊、文件传输、设置定向测试，Windows 原生 Release 全量构建，真实 QUIC 回环：1 MiB 文本 1776 wire 字节，300 KiB 随机数据 307245 wire 字节（5 个原始块多 45 字节）；完整 ctest 70/70 通过 | 只用于文件传输/下载；小文件、剪贴板文件和续传保持 v1；每块独立 zstd level 1，已压缩容器无收益；两端必须同为 v66。详见 [文件传输压缩](file-compression-2026-11-10.md) |
| 文件同名冲突（v69） | 已接入 | 主机在目标已存在时发送 `FileConflict`（identical 位/现有大小/修改时间），查看端弹窗跳过/覆盖/取消并可记住本队列选择；`FileConflictAnswer`、`FileCode::Skipped`/`Conflict` 与双向（上传/下载）持有逻辑；协议往返负向用例、`file_transfer` 的 inspect/overwrite/跳过用例、`viewer_probe --file-conflict` 真实会话场景通过 | 弹窗 UI 只在单机会话中跑过探针场景，未做双机人工操作；等待决定期间主机不设超时，队列跨方向共用单一记住策略 |
| 剪贴板初始同步与 CCLB v3（v69） | 已接入 | 基准建立后主机主动发送一次当前剪贴板；CCLB v3 每条目可选 zstd（≥256 字节、压缩必须变小），头记录 stored/raw 长度，解压长度必须等于声明；`clipboard`/`clipboard_async` 与 `host_parity` 剪贴板场景通过 | 本机共享同一系统剪贴板，接收方向被自回声抑制；真实双机与 PNG/大图像待验收 |
| 指针复核（P2，2026-11-11） | 部分完成 | 仅查看远端指针修正 DXGI 无鼠标更新帧误判并 GUI 复核；控制模式自定义光标与隐藏语义代码路径完整；精灵上限 512 px；所有者标记链路已通（查看端 33 ms 提示→主机 `chast_cursor_hint` 窗口） | 所有者标记仍有 32×32 黑底（`LWA_ALPHA`），待改逐像素透明；OS 级隐藏指针与 512 px 真实缩放未验收；白板未实现。详见 [指针四项复核](pointer-acceptance-2026-11-11.md) |
| 自定义画质百分比与自定义 FPS（G15，v67） | 已接入 | 协议 v67 往返/边界拒绝/旧 5 字节请求拒绝；`video_qos` 自定义与预设最新投票互切；`host_parity` 真实 QUIC 下 75%/25% 映射到 High/Smooth 码率且结果回报实际百分比；工具栏/偏好/状态定向测试、`flutter analyze`、全量 Flutter 710/710、Windows 观看端 Release 构建 | 数值入口按设备保存；自定义画质仍受主机 ABR 自适应和 0.1 下限约束；真实双机联动与弱网观感未验 |
| 显示器窗口布局恢复（G15，2026-11-10） | 已接入 | 原生布局验收程序覆盖往返、同键替换、坏行/多余字段拒绝、离屏忽略、256 条上限与 Roaming 路径；Windows 观看端 Release 构建 | 仅 Windows 观看端；物理像素；键为服务器命名空间下的设备 ID + 显示序号；双机/多显示器人工目视待验。详见 [同一会话的显示器窗口](shared-display-windows-2026-11-09.md) |
| 集中策略对象与分配（G04，schema v14） | 已接入 | 存储/API/管理台自动化、`go build ./...`、`go vet`、`go test ./...`；真实服务端 + 代理回环 26/26（注册下发、设备范围覆盖、回退单调版本、签名回执、解除受管）；真实浏览器验证三块页面、设备/分组范围分支与删除覆盖后的回退 | 真 `chast-host.exe` 受管会话整链、Postgres 实例、并发编辑与大规模设备重算未验收；本地设置只能收紧中央上限的既有边界不变。详见 [受管准入与会话授权设计](access-control-design-2026-11-09.md) 第 8 节 |
| 光标独立投递与全队列背压（H20，F10 补充） | 已接入 | `desktop_stream` 422 项断言连续 3 次通过；`host_parity`（189.15 s）等定向套件通过；全量 Release ctest 70/70（1148.07 s）；真实路径静止 8 s `frames=236 encoded=234 pointer-only=0`、`cursor sent=1`；移动 8 s `frames=228 encoded=226`、`cursor sent=219/0 丢弃`；500 kbps/30 ms netem 470 AU 全送达、最大帧年龄 4116 ms、光标 `sent=156/0 丢弃` | 光标 sink 依附采集节拍而非独立线程；真实双机高延迟/丢包下的节拍、形状切换与坐标精度待验。详见 [主机 RustDesk 对照](rustdesk-host-parity-2026-11-07.md) F10 补充 |
| 弱网码率恢复判据与静止刷新 QoS 复核（2026-11-11） | 已修正 | 恢复判据改为“最低目标之后出现过更高目标通告”；撤销“静止刷新帧计入共享 QoS”的错误对齐（RustDesk `static_refresh.try_encode` 不增加 `send_counter`，固定基线 `7423dce` 无该模块），刷新帧不计入动态画面证据，只有真实捕获帧驱动回升；撤销后 `video_qos`/`desktop_stream`/`quic_weak_network` 定向 3/3，弱网直跑 2 次通过（496/492 个访问单元，最低 1000 kbps、回升 1749 kbps、30 fps），全量 Release ctest 70/70（917.55 s）；`desktop_stream` 饱和基线仍等待队列高水位后取样 | 单机 netem 回环，测试桌面有持续真实更新（约 20 fps）；真实双机弱网观感与完全静止桌面的恢复时机待人工验证。详见 [共享视频质量](shared-video-qos-rustdesk-2026-11-08.md) |
| 调价入口节奏对齐（2026-11-11） | 已修正 | `VideoQos::tick` 增加 1 秒周期入口节流（完整调价仍保持 3 秒冷却），与 RustDesk `update_display_data` 一致；新增确定性 `video_qos` 断言，去掉节流即失败；`video_qos` 110 项、`desktop_stream`/`quic_weak_network` 定向 3/3 通过；全量构建修复 `protocol_test` 的 `preset_percent` 重复定义 | 单机确定性测试；真实双机弱网下回升时机未单独观测。详见 [共享视频质量](shared-video-qos-rustdesk-2026-11-08.md) |
| 管理应急访问入口（G02/G03，2026-11-11） | 已接入 | Flutter 会话页应急对话框→tunnel stdin→`ready`/`access_ticket` 事件→原生票据交接；Dart tunnel 18/18、`flutter analyze` 无问题；Go 服务端签发/拒绝/审计（`TestEmergencyAccessIssuesShortLivedTickets`、`TestEmergencyAccessRequiresExplicitAdminRequest`）与查看器受限设备准入（`TestEmergencyAccessViewerReachesRestrictedDevice`）通过；直连/载体路径由 20 秒凭据刷新循环续签票据与三份传输凭证，`TestEmergencyAccessRenewsTicketWithoutRelay` 断言无 relay 时新票据到期时间后移且主体不变 | 管理台只提供审计快速筛选（票据绑定查看器密钥，控制台不能代签）；真 `chast-host.exe` 受管会话整链与多操作者审计汇聚仍待验收。此前一次真实链路手工验收（本机 server + 真实代理）未保留日志，不计入可复核证据，需重跑并另存证据 |
| 查看端光标提示（G21，v68） | 已接入 | 协议往返/负坐标/隐藏/截断/尾随/未知标志/保留位用例、主机覆盖层构建、查看端工具栏开关与按设备偏好自动化通过；独立显示器窗口复用同一按设备偏好、33 ms 节流、帧→桌面坐标映射与离开/关闭隐藏（`display_window_preferences_test` 断言精确坐标和偏好关闭后停止发送）；`flutter analyze` 无问题，`display_menu`/`view_prefs` 定向 57/57；全量 Release 构建与 `ctest` 70/70 通过 | 白板绘制未实现；覆盖层在真实多屏、混合 DPI、负坐标和 Windows 10 上的目视位置待人工验证；坐标不进入注入器 |
| 主机语音麦克风选择（G22） | 已接入 | `host_settings` 覆盖往返/空值/超长值拒绝/拔出设备保留；`host_voice` 用合成采集器断言先请求固定 ID、失败后用空 ID 回退并成功出包；全量 Release 构建与 `ctest` 70/70 通过 | 本机无物理麦克风，真实录音、设备切换和双机听感待人工验证；通话进行中不切换设备 |
| 会话期间移除壁纸（G23） | 已接入 | `host_settings` 覆盖空路径拒绝、缺失备份、无壁纸备份丢弃和挂起备份阻止二次移除；`host_voice` 断言桌面会话只创建一次移除器并在会话结束时恢复；全量 Release 构建与 `ctest` 70/70 通过 | 真实壁纸视觉切换、异常断电恢复和混合 DPI 待人工验证；默认关闭，只降低静止画面编码成本，不改变零拷贝与 QoS |

## 3. 当前协议与关键实现位置

- 协议：`include/chast/protocol.h` / `src/net/protocol.cc`，`kProtocolVersion = 69`；Hello 已含能力、codec 偏好、色度和各权限位；v63 新增 `InputKind::LegacyKey`（Windows 虚拟键码），v64 新增 `FileOperation::RemoveTree`（文件夹递归删除），v65 为 `FileListRequest` 增加 flags 字节（bit0 显示隐藏项），v66 为 CFIL 增加 v2 压缩块流（`FileStreamHeader.version = 2`、9 字节 `FileBlockHeader`、zstd 按块压缩与原始回退；v1 仍受支持），v67 为 `SetStreamOptions` 和 `StreamOptionsResult` 增加画质百分比字节（`StreamQuality::Custom` 携带 10..100，预设必须为 0；结果回报主机实际生效的百分比），v68 新增 `ControlType::ShowCursor`（83，虚拟桌面坐标与可见标志，主机只保留最新提示），v69 新增 `ControlType::FileConflict`（84，主机在目标同名时替代 `FileAnswer` 等待决定）与 `FileConflictAnswer`（85，跳过/覆盖/取消），并为 `FileCode` 增加 `Skipped`（7）/`Conflict`（8）、为 CCLB 增加 v3 逐条 zstd（`stored/raw` 双长度与压缩标志）。
- 主机认证与权限合成：`src/host/host_security.cc`、`include/chast/host_security.h`；本地密码、审批、TOTP、可信设备、白名单和权限位裁剪，以及受管设备的签名票据准入、20 秒续租与权限交集（票据 ∩ `permission_ceiling` ∩ 本机默认 ∩ 查看器请求）。
- 会合与票据签发入口：`server/internal/api/api.go`；管理应急访问为 `POST /v1/emergency-access`（`server/internal/api/emergency_access.go`）；查看器 lookup 经 `server/internal/agent` 的 `chast-tunnel viewer` 完成，Dart 侧为 `app/lib/tunnel_agent.dart`、`app/lib/server_api.dart`。
- 账号与组：`server/internal/account/store.go`、`groups.go`、`api_tokens.go`、`policy_objects.go`、`migrate.go`；当前 schema 版本 14（v10 增加设备策略，v11 增加 `api_tokens` 机器令牌，v13 增加策略对象/分配，v14 增加按设备单调策略版本序列），`device_policy.go` 与 `/v1/device-policies` API 已有自动化，管理台“设备策略”页可按回执显示“已应用版本/待应用”。G04 策略对象/分配已落地：`policy_objects` 存名称唯一、模式、权限上限、允许用户/组与对象版本；`policy_assignments` 把对象绑定到 `all`/`group`/`device` 之一（同一范围唯一，优先级 -10000..10000）；解析按优先级降序、同优先级按设备 > 分组 > 全部，结果物化到 `device_policies` 并记录来源，手动设备编辑无来源且覆盖所有分配，删除后回落到匹配分配。分配或对象变更会重算受影响设备；删除仍被分配引用的对象返回 409。每次物化的设备版本由 `device_policy_versions` 单调分配，回退或重建不会回退版本。管理 API 为 `GET/POST /v1/policies`、`GET/PUT/DELETE /v1/policies/{id}`、`GET/POST /v1/policy-assignments`、`DELETE /v1/policy-assignments/{id}`（admin `devices` 资源），写操作记审计。服务端在注册响应中下发设备策略，Go 代理已解析并经 `HostInfo.Policy`/`chast-tunnel` 事件上报，并把策略原子写入与主机共享的 `<stem>.policy.json`；C++ 每个新会话读取该文件，受管或损坏时失败关闭（`Declined`），并在共享标记匹配后写 `<stem>.policy-ack.json`，Go 代理据此签名上报应用回执。控制台机器令牌按 `devices/audit/accounts/groups/recordings/ops/all` 资源集合授权，组范围已覆盖账号组、组审计、设备与录制（设备列表/策略按组成员过滤，单设备操作与应急访问逐目标校验；录制按所有者组成员或目标设备组成员过滤），`accounts`/`ops` 仍为全部署面，见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md)。权限上限交集与票据准入已在前述票据链路落地（权限逐位取票据 ∩ `permission_ceiling` ∩ 本机默认 ∩ 查看器请求）。撤销推送也已落地（2026-11-10）：schema v12 增加 `revoked_access_kids` 持久表，管理 API `GET/POST /v1/access-kids` 退役签发 `kid`（幂等、只管理 admin、记审计）；签发端点一旦发现当前签发 `kid` 已退役即返回 503。主机事件流 `GET /v1/hosts/{id}/events` 以共享主机凭据 + 设备密钥对 `(hostID, after, timestamp)` 的 Ed25519 签名做长轮询，按主机推送 `policy`（策略版本变化）与 `revoke`（`kid` 退役）事件；Go 代理消费事件、写共享策略文件并向主机 stdout 输出 `policy`/`revoke`；C++ 主机对票据准入的受管会话按“新策略版本”或“同签发 `kid`”立即断开（续租会刷新匹配元数据），无关语句不影响会话。推送只缩短撤销延迟，正确性仍由 60 秒票据 + 20 秒续租的 65 秒上限保证（`host_events_test`、`host_ticket_lease` 与真实回环覆盖）。2026-11-10 真实链路验收（生产 `chast-server` + 真实 `chast-tunnel` 代理，本机端口 18543）16/16 通过：受管策略 v1 注册下发、策略 v2 推送落盘、`kid` 退役推送、签发 503、退役列表持久可查；长轮询已响应请求取消，避免客户端断开后占住 handler 到 25 秒超时。G04 真实链路另用 18743 端口 26/26 通过：策略分配注册下发、设备范围覆盖、删除分配回退到单调 v3、签名回执与被解除受管。剩余边界：真 `chast-host.exe` 进程与真实受管会话的整链撤销仍属环境验收，代理 stdout → 主机进程的接线由 `host_tunnel_test`/`host_ticket_lease` 覆盖。账号组仍只服务控制台范围。
- 录制：主机入站自动录制已完成；查看端出站自动录制、容量守卫、失败提示和可选保留策略已接入（默认不自动录制、不自动删除），见 [查看端自动录制](viewer-auto-record-2026-11-10.md)。
- 键盘：物理映射（Map 方向）、Legacy（v63 虚拟键码）与本地输入法（Translate 方向）三种模式均已实现；非 US 布局/AltGr/死键/其他 IME/安全桌面仍待实测。

## 4. 后续计划

### 第一优先：G02 连接 ACL（企业准入前必做）

目标：账号/组 → 设备策略，禁用设备或账号、移组、过期票据均能拒绝；直连、打洞和经服务器路径都明确受管，不允许回退绕过。

计划分片（修订约束见 [受管准入与会话授权设计](access-control-design-2026-11-09.md)；第 1、3、5 片、撤销推送、查看器账号 lookup、打洞/relay/carrier 传输凭证与管理应急访问查看器入口均已完成；应急访问的真实链路手工验收证据未保留，待重跑，剩余真主机进程整链与 G03 收尾）：

1. ✅ 账号存储迁移 v10（SQLite 与 Postgres 同步）：设备策略 `open/restricted`、允许用户/组、权限上限、策略版本与应用回执；区分未纳管和受管设备。`device_policy_test.go` 与 `device_policies_test.go` 覆盖未纳管默认、ACL、禁用账号、版本递增、过期回执拒绝、重启持久化及 admin-only 管理 API；回执需设备注册公钥签名。管理台“设备策略”页（仅 admin）可新建/编辑/删除策略，回执版本等于当前策略且 applied=true 才显示“已应用版本 N”，否则显示“待应用/应用失败”；`webadmin_test.go` 锁定页面与回执语义，真实浏览器验证了新建与编辑。主机侧已能产生真实回执：C++ 在共享策略标记匹配后写 `<stem>.policy-ack.json`，agent 用设备密钥签名异步上报。**边界：受管设备当前对所有新会话失败关闭，回执只表示主机已确认策略，不表示票据准入已可用。**
2. 🔶 lookup 显式解析单一身份并检查设备策略：restricted 已拒绝普通 link key 与管理 key，账号 token 按 ACL/禁用状态准入，batch 返回 `denied`。独立短期传输凭证已完成打洞段（2026-11-10）：lookup 接受 `transport_public_key`，按 punch/relay/carrier 三个 audience 各签发 60 秒凭证（域前缀 `chast-transport-ticket-v1`，绑定主机、客户端临时公钥、主体、策略版本与截止时间）；UDP/TCP 打洞节点用固定 issuer 公钥验签并校验查看器对 nonce/时间戳的签名证明，受管设备一律拒绝 link/server key HMAC 回退，策略新增/删除即时同步 rendezvous 状态。Go 测试覆盖凭证格式/黄金向量、lookup 签发、受管打洞拒绝旧凭据、受管与未纳管代理回环（`transport_test.go`、`transport_punch_test.go`、`transport_tickets_test.go`、`agent/transport_punch_test.go`）。relay/carrier 已切换到传输凭证（2026-11-10）：内置 relay 端口、共享 carrier（TCP/WS）与独立 `chast-relay` 都验证 `transport:relay`/`transport:carrier` audience 的短期凭证及查看器签名证明（nonce/时间戳 ±30 秒，有界重放缓存绑定来源地址），受管设备拒绝旧 carrier token；独立节点由 hbbs 在每次续租刷新 `managed`（策略解析先于租约分配，首次续租即携带新值）；策略变更时还会对持有活动租约的节点异步 `POST /v1/leases/{id}/managed` 立即翻转准入标记（只改标记，不动密钥、端口、到期或载体令牌），节点无活动租约时返回 404 并由下次注册收敛，不再等主机注册 TTL（`TestManagedUpdateAppliesToLiveLease`、`TestSetManagedTargetsTheHoldingNode`、`TestPolicyChangePushesManagedToHoldingNode`）。Go 代理在剩余有效期不足 30 秒时经 lookup 刷新并重签。自动化覆盖验签、错 audience、坏证明、跨源重放、受管拒绝旧 token 与受管直连（含打洞）、relay、carrier 回环（`relay/transport_test.go`、`relaynode/auth_test.go`、`agent/transport_punch_test.go`），真实 relay 节点与跨网链路仍未人工验收。管理应急访问的显式授权/审计已在服务端落地（2026-11-10）：`POST /v1/emergency-access` 只接受账号 admin 或部署管理 key，要求非空原因（≤512 字节），绕过设备允许名单但仍保留权限上限、主机本地认证和 60 秒票据/续租约束，同时签发访问票据与三份传输凭证；审计使用独立动作 `emergency_access`，主体为 `emergency:<actor>`（部署管理 key 为 `emergency:server-key`），`emergency_access_test.go` 覆盖票据/凭证验签、权限上限、管理 key 主体与匿名/非管理员/缺原因/超长原因/坏公钥/未知主机/未纳管/未配置签发的拒绝。查看器隧道已用登录账号 token 认证 lookup（2026-11-10）：Dart 在账号与目标部署一致时经 stdin 传入 token，agent 以账号身份准入受限设备，link key 只留作无传输凭证部署的打洞回退；`TestRestrictedDeviceLookupWithAccountToken`/`TestAccountLookupKeepsLegacyPunchFallback` 覆盖。管理应急访问的查看器入口已接线（2026-11-11）：会话页“管理员应急访问”对话框用管理员账号登录取得 token 与原因，经 tunnel stdin（`--emergency-token-stdin`/`--emergency-reason-stdin`/`--emergency-client-key`，token 与原因不进 argv）申请覆盖；tunnel 的 `ready`/`access_ticket` 事件携带票据、主体、权限与到期时间，Dart 跟随续租事件把新票据交给原生查看器，票据续租失败不打断会话但会话仍受原票据 60 秒截止约束。`chast-tunnel` 每次传输凭证刷新都会重新签发覆盖并再记一条 `emergency_access` 审计；直连与 carrier 路径另由 agent 的 20 秒凭据刷新循环续签访问票据与三份传输凭证（`TestEmergencyAccessRenewsTicketWithoutRelay` 覆盖无 relay 时新票据到期时间后移且主体不变）。此前一次真实链路手工验收（本机 server + 真实 host/viewer 代理、restricted lookup 403、票据验签、relay 凭证续期与回显、非管理员 403）未保留日志，不计为可复核证据，需重跑并另存证据。管理台无法安全代签票据（票据绑定查看器自持密钥），因此只提供“应急授权”审计快速筛选入口；多操作者审计汇聚与真 `chast-host.exe` 受管会话整链仍待验收。**边界：主机仍是唯一执行点，客户端从直连地址连接时可绕过 lookup；必须等主机策略同步/守门落地后才算 ACL 生效。**
3. 🔶 独立服务端 Ed25519 私钥签发，主机固定公钥；票据绑定 issuer/audience/kid、主机 ID 与设备公钥指纹、主体、客户端持有证明、权限、策略版本、时间和 jti。规定重放消费、轮换、字节编码与跨语言黄金向量；禁止部署 host_key 参与签发。已完成 Go 侧 `server/internal/acl`：v1 字节格式冻结、签发/严格解析/验签、未知 kid 拒绝、60 秒与 ±5 秒时间边界、有界重放缓存及黄金向量；已完成 `POST /v1/access-tickets`（账号 token 换 60 秒票据，绑定设备指纹/账号/客户端公钥，权限与策略上限取交集，`jti` 进重放缓存，`--access-issuer/--access-kid/--access-key` 配置），端点测试已通过。C++ 校验库也已落地（`include/chast/access_ticket.h`、`src/host/access_ticket.cc`）：原始 envelope 的 Ed25519 验签、固定 `kid`、主机/指纹绑定、±5 秒窗口和有界重放缓存，`chast-access-ticket-test` 复现冻结黄金向量并覆盖篡改/截断/未知 `kid`/重放，已构建进 `chast.host`。主机准入已接入 `HostSecurity`：`host.ini` 新增 `access_issuer`/`access_kid`/`access_public_key`（base64url Ed25519 公钥，`host_settings_test` 覆盖校验与往返），设备 ID 取 `tunnel_id`；受管设备只接受验签通过、版本等于共享标记且未重放的票据，权限逐位取票据 ∩ `permission_ceiling` ∩ 本机默认 ∩ 查看器请求，缺票据/缺固定项/坏标记/过期/重放/错设备/错指纹/版本不符全部失败关闭（`chast-host-security-test` 覆盖黄金票据及否定路径）。指纹比较忽略 `SHA256:` 前缀、分隔符与大小写，服务端签发时把裸摘要规范为 `SHA256:<小写十六进制>`。已完成 v62 携带票据、挑战绑定、lookup 返回票据与主机/客户端库续租（`TicketRenew`/`TicketRenewResult`，同主体与同客户端公钥、新 `jti` 通过才移动截止、权限只减不增、拒绝即断开；`host_ticket_lease` 三阶段覆盖）；查看器已落地 DPAPI 持久化 Ed25519 客户端身份、启动时在线取票与 20 秒自动续租（`app/test/server_api_test.dart` 覆盖票据 lookup，原生代码经 `chast-viewer-probe` 与 `chast_viewer` 构建验证）；主机已支持重叠信任多个签发 kid（`host.ini` 可重复 `access_key=<kid>:<base64url>`，主 pin 与最多 8 个附加 kid 同时生效，`host_settings_test`/`host_security_test` 覆盖格式校验、往返、新旧 kid 准入与退役 kid 拒绝）。撤销推送与在线终止已完成（2026-11-10）：`revoked_access_kids`（schema v12）+ `POST /v1/access-kids` 退役签发 kid，签发端点对被退役 kid 返回 503；`GET /v1/hosts/{id}/events` 长轮询按主机推送 `policy`/`revoke`，Go 代理转 `policy`/`revoke` stdout 事件，C++ 主机按新策略版本或匹配签发 kid 立即断开票据准入会话（`host_events_test`、`applyHostEvent` 单测、`host_ticket_lease` 回环与 API 集成测试覆盖）。生产 `chast-server` + 真实代理的 16/16 链路验收已通过（策略推送落盘、`kid` 退役推送、签发 503、持久列表）；真 `chast-host.exe` 进程 + 真实受管会话的整链验收仍属环境项。
4. 🔶 G02 前置最小受管策略同步：认证安装、持久化、入口守门、应用确认；后台在确认前显示待应用。受管配置损坏/失联不能回退 open，旧客户端拒绝；直连、各打洞/中继、服务 worker 和全部会话模式共用 HostSecurity 守门。已完成服务端注册响应下发 `device_policy`（读取失败以 503 拒绝续租）、管理台回执展示，以及 Go 代理解析该字段并经 `HostInfo.Policy`/`chast-tunnel` 的 `device_policy` 事件上报；代理还把策略原子写入与主机共享的 `<stem>.policy.json`（启动载入、写失败重试、解除纳管写 `managed=false`，`device_policy_test.go` 覆盖），C++ 侧派生路径并传 `--policy-state`（`host_settings_test`/`host_tunnel_test` 覆盖），`HostSecurity` 对受管设备的所有新会话失败关闭（`host_security_test` 覆盖缺失/未纳管/受管/损坏）。签名回执闭环已完成：C++ 在共享标记匹配时写 `<stem>.policy-ack.json` 并经 `--policy-ack` 传给 agent，agent 校验版本/模式/更新时间精确匹配后用设备私钥签名异步上报，失败下次注册重试（`device_policy_test.go` 覆盖确认匹配与陈旧确认拒绝）。权限上限交集与票据准入已接入 Hello 守门（票据 ∩ `permission_ceiling` ∩ 本机默认 ∩ 查看器请求，`host_security_test` 用黄金票据覆盖）；v62 Hello 携带票据与主机侧续租已落地（`host_ticket_lease` 覆盖到期、续租、拒绝与权限收窄四阶段，收窄后新文件列表被主机拒绝），查看器已在启动时取票并按 20 秒自动续租；账号/设备被拒后无法取得新票，存量会话最迟在原票据 60 秒截止处结束；撤销推送已实现（`policy`/`revoke` 事件经 `GET /v1/hosts/{id}/events` 长轮询下发，主机立即结束匹配会话），推送只缩短延迟，65 秒上限仍由租约保证。独立 relay 节点的 `managed` 标记也会在策略变更时立即推送（`POST /v1/leases/{id}/managed`，只改标记），无活动租约时由下次注册收敛。
5. v62 扩展票据、客户端挑战证明与会话续租。票据/策略缓存/租约不超过 60 秒，每 20 秒重新授权，时钟容差最多 5 秒；账号/设备禁用或移组后的旧票据及存量会话最多 65 秒失效。失联不延长期限，撤销推送仅缩短延迟。
6. 查看器将账号 token 经 stdin 交给 Go agent（`--account-token-stdin`）；应用票据由 Dart 侧账号 lookup 获取后进入原生握手，独立传输凭证留给 agent；token 不进入 argv/日志，短期票据不落盘，重连重新授权。已落地（2026-11-10）：仅当登录账号与目标部署一致时传 token，agent 以账号身份 lookup（受限设备可准入）；agent 每次启动生成临时 Ed25519 密钥，lookup 申请并持有 60 秒传输凭证，UDP/TCP 打洞、relay、carrier 出示凭证与持有证明；未签发凭证的部署回退旧 link key，link key 与账号 token 不混用。
7. 验收覆盖新 lookup、先取票再撤权、已连接后撤权三种状态，逐一验证直连、UDP/TCP 打洞、relay、TCP/WS carrier；加入失联、重启、时钟变化、配置回退、ID 重用、密钥轮换和旧客户端拒绝。未纳管 open 保持旧行为，受管 open 也需要短期中央凭证。

完成标准对应 [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md) 的 G02 行：不允许通过直连地址或旧 link key 回退绕过。

### 第二优先：G03 账号级会话权限票据

沿用 G02 的独立签发和租约框架，权限取票据上限、当前设备策略、主机本地策略和用户同意的交集；接入全部敏感入口。撤权须停止正在执行的终端、转发、文件等操作并释放资源，同样满足最多 65 秒上限。与 G02 分开验收；不要求中继解密桌面数据。

状态（2026-11-10）：源码侧权限交集与运行中撤权已覆盖。主机 handler 在出队时重读会话掩码（重启/锁屏/阻止输入、远程配置、OS 登录、会话切换、隐私模式），transport 对终端/端口转发同样准入校验；撤权会释放按住键、中止麦克风/摄像头/剪贴板/文件流、断开终端与端口转发、清空目标注册，桌面循环同时解除本地输入阻止并关闭隐私模式。自动化：`host_parity --revocation` 在已收发的回环端口转发和已运行的 ConPTY 控制台上撤销两项权限，验证查看器能力更新、后续收发被拒、主机转发套接字关闭、主机日志 `terminal access was revoked` 且控制台进程被 relay 停掉；阻止输入/剪贴板/文件/摄像头/麦克风/隐私模式另有既有测试。撤销推送已实现：服务端按主机长轮询下发 `policy`/`revoke`（schema v12 的 `revoked_access_kids` 持久化退役 kid，签发端点对被退役 kid 返回 503），Go 代理写共享策略文件并向主机 stdout 转 `policy`/`revoke` 事件，C++ 主机按新策略版本或匹配签发 kid 立即断开票据准入会话；生产服务端 + 真实代理的 16/16 验收已通过，真主机进程与真实受管会话的整链验收仍属环境项。

### 第三优先：G04 集中策略（已完成）

策略对象、`all`/`group`/`device` 分配、优先级与同优先级“设备 > 分组 > 全部”决胜、手动覆盖与删除回退、物化版本单调、事件推送和管理台三块界面均已落地；真实服务端 + 代理回环 26/26 通过。中央策略仍只经 G02 的 `device_policies` + 共享策略文件 + 票据准入执行，没有新增旁路；本地设置只作为设备侧收紧。剩余边界为真 `chast-host.exe` 受管会话整链、Postgres 实例和并发编辑/大规模重算验证。详见 [受管准入与会话授权设计](access-control-design-2026-11-09.md) 第 8 节。

### 之后按顺序

1. ✅ G09 多窗口/多主机剪贴板真实 GUI 复核：双主机双会话同进程、首页关闭后逐窗口发送与独立关闭已通过；仅剩双机真实接收方向（本机无第二台设备）。
2. ✅ G10 键盘 Legacy：v63 已实现虚拟键码模式，保留 Map 默认和本地输入法/Translate 方向；非 US 布局、AltGr、死键、Unicode/IME 与快捷键混合的实测仍待对应设备执行。
3. ✅ G11 自动录制：出站查看器自动录制、存储容量/失败提示/保留策略已实现；集中留档仍由受管主机负责并报告失败。真实会话与磁盘满场景待人工验证，见 [查看端自动录制](viewer-auto-record-2026-11-10.md)。
4. ✅ G12 管理权限颗粒度：资源集合 × 角色动作、账号组/组审计/设备/录制组范围、独立机器令牌的创建/到期/撤销/最近使用/审计已实现；管理台“API 令牌”页（仅 admin 账号可见）已接线并完成本地真实浏览器创建/撤销冒烟，真实部署升级和 Postgres 未验收，见 [管理权限与 API Token](admin-api-tokens-2026-11-10.md)。
5. ✅ G18 递归删除（v64）、隐藏项（v65）、多共享根（v65）与文件压缩（v66）：文件夹确认与整树删除、隐藏项开关/分页前过滤/递归下载沿用视图、多共享根/盘符与顶层别名、CFIL v2 按块 zstd 压缩与原始回退均已实现并通过真实 QUIC 回环；G18 文件管理范围关闭。
6. ✅ G15 自定义画质百分比/帧率（v67）与显示器窗口布局恢复：数值入口、按设备偏好、最新投票互切、真实 QUIC 百分比码率映射和原生布局持久化均已通过定向测试与构建；双机/多显示器人工目视仍待执行。
7. G13 就近中继、G14 LDAP：按部署需求单独立项，不用合成/单机结果替代真实多地区或目录服务验收。
8. ✅ G21–G23 宿主 P3（2026-11-11 已实现）：显示我的光标（v68，光标部分；白板绘制未实现）、主机语音麦克风选择（设置/托盘、按呼叫读取、缺失设备回退）、会话期间移除壁纸（默认关闭、备份/恢复/异常恢复）。三项都不影响主链路；自动化与构建证据见第 2 节，物理麦克风、壁纸视觉切换、白板绘制和多屏覆盖层位置待人工验证；截图来源差异并入 C40 的部分对齐，不单独编号。
9. 🔶 P2 指针四项（2026-11-11）：仅查看远端指针已修正并 GUI 复核，控制模式自定义光标和隐藏语义代码路径完整，精灵上限 512 px，所有者标记链路已通；剩余为覆盖层 32×32 黑底、OS 级隐藏指针与 512 px 真实缩放验收，见 [指针四项复核](pointer-acceptance-2026-11-11.md)。
10. P1 文件浏览空目录上传、剪贴板 PNG/大图像处理与双机弱网/G02/G03 端到端列在后续，分别依赖代码与小修和两台机器环境。

### 环境相关验收（不是代码缺口）

真实 UAC、跨管理员用户、多用户 RDP、多物理屏/混合 DPI/多 GPU、物理摄像头、真实 HDR、Explorer 粘贴、其他 IME、跨公网 NAT/IPv6/代理/中继、弱网和不同显卡故障恢复，仍需在对应设备或网络环境执行；测试前记录设备、步骤、预期和证据边界。

## 5. 协作与验证约定

1. 文档分开记录“已实现”“已构建”“自动化通过”“真实链路通过”“待人工验证”；模拟媒体、故障注入和 dry-run 只证明其覆盖的边界。
2. 协议字段只增量添加；语义变化升版本，并同步两端解析、非法输入处理和文档。
3. 第三方依赖通过 vcpkg 管理并静态链接；不复制 DLL，不引入 RustDesk 代码。
4. 本机状态、PID、身份密钥、证书、日志、构建产物和临时验收程序不提交；测试凭据不进入仓库。
5. 仅在用户明确要求时提交或推送；推送保持 fast-forward，不改写历史。

## 6. 恢复入口

```powershell
git switch main
git log -1 --oneline
git status --short

# 原生构建与关键回归（依赖前缀见检查点文档）
cmake -S . -B build -DVPL_ROOT=D:/vpl_install_static -DFFMPEG_ROOT=D:/vcpkg/installed/x64-windows-static -DOPENH264_ROOT=D:/vcpkg/installed/x64-windows-static-md -DVPX_ROOT=D:/vcpkg/installed/x64-windows-static-md -DOPUS_ROOT=D:/vcpkg/installed/x64-windows-static -DSPEEXDSP_ROOT=D:/vcpkg/installed/x64-windows-static -DMSQUIC_ROOT=D:/vcpkg/installed/x64-windows
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure

# Go 服务端
cd server; go test ./...

# Flutter 查看器
cd app; flutter analyze; flutter test
```

阅读顺序：本文件 → [开发检查点](development-checkpoint-2026-11-09.md) → [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md) → 对应模块文档。
