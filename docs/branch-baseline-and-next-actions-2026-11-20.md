# 分支基线与下一步执行计划

日期：2026-11-20；最新审核补充：2026-11-27

当前开发分支：`codex/complete-workspace-2026-11-20`

2026-11-27 H01 本机审批→后台 manager owner 接线：便携主机现在在 UI 线程创建 `PortableHelperLocalApprovalGate`；远端每连接请求只进入本机审批队列，本机批准时重新核对 viewer、桌面模式、键鼠控制租约和实时输入权限，随后将 move-only receipt 交给后台 `PortableHelperElevatedManagerTask`，不阻塞界面等待 UAC。便携模式的 helper 请求使用独立 `Available` 状态，不调用旧 `ElevationController` 的整主机接管；审批窗口明确说明仅对该连接启用临时桌面读取，不授权键鼠输入。Manager task 仅签发 capture capability，串行化 Capture/Stop，WorkerReady 后才回报 `HelperReady`，断开/撤权/退出会取消并停止 manager。Release `chast-host`、parity 与 lifecycle 目标构建通过；`--elevation` 子进程套件通过，`portable_helper_authorization` 和 `portable_helper_lifecycle` CTest 各重复 5 次 **10/10 PASS**。尚未把 manager capture RPC 接到 viewer 的画面路径，未接真实 UAC、SCM/SYSTEM/ProgramData 或真实桌面；发布者 pin 未配置，产品 staged launch 仍失败关闭，键鼠 grant 继续关闭。下一项接通 per-viewer capture/停止生命周期和可见状态反馈，再按实际签名/WDK/隔离管理员验收条件推进产品服务与 source-isolated 输入。

2026-11-27 H01 HelperReady 状态分离：协议 v79 增加只表示单连接 helper Ready 的状态码；HostServer 不再把它作为 task 终态，保留原 viewer 的生命周期 owner，重复请求回 Busy 并在断开时取消。Flutter 单独提示 helper 已对该连接就绪，不再与整主机 Elevated 状态混淆。Release host/parity/protocol 构建、原生 protocol test、`--elevation`、Flutter 相关用例 15/15、analyze 与 `git diff --check` 通过。尚未接入 gate receipt、manager、SCM 或 capture/input 数据路径；无真实 UAC/桌面证据，发布签名 pin 和 source-isolated 输入条件未满足，产品键鼠 grant 继续 fail-closed。下一步实现 UI 批准 receipt 到后台 manager owner 的任务状态转换，并以 HostViewer 终止/撤权回调驱动 Stop。

2026-11-27 H01 parent manager 生命周期复核：事务 API 现在在 receipt 消费后、manager 身份/管道认证后 grant 发送前、WorkerReady 后和每次 Capture/Input RPC 前复核原连接；viewer 失效时拒绝代理并 Stop manager。新增隔离 manager 子进程回归，验证 UAC 启动窗口断线不会收到 grant、Ready 后断线由下一次 RPC 触发 Stop 且等待 cleanup ACK 和进程正常退出。Release host/parity/lifecycle-test 构建通过；授权与生命周期 CTest 各重复 5 次 **10/10 PASS**，elevation 组合套件通过，`git diff --check` 通过。此事务 API 尚未接入 `host/main.cc`，所以空闲期断线需由后续 HostServer lifecycle owner 主动 Stop；viewer `Elevated/Restarting` 当前表示整主机 handoff，不可冒充 per-connection helper Ready。publisher pin 和 source-isolated 输入后端未具备，产品键鼠 grant 保持 fail-closed。下一步接入独立 helper 状态、原 viewer 撤权/关闭回调和异步 manager owner，再接 capture/input 数据路径。

2026-11-27 H01 cleanup executor 加入 fail-closed 计划验证：执行前拒绝非 key-up/未标记键、鼠标非 button-up、touch 非 canceled-up、异常 pen 数据、重复对象及超限计划；失败在 desktop/输入 API 前返回。Release host/service/parity 构建成功，高完整性授权套件通过，授权与生命周期 CTest 各重复 5 次 **10/10 PASS**。这是调用边界与错误输入回归，不代表真实桌面清理或物理键冲突已解决，产品键鼠 grant 继续关闭。

2026-11-27 H01 cleanup child 强杀后跨进程恢复回归：在 Release parity harness 中启动独立 child，使用受保护隔离文件 journal 和注入式副作用计数器；child 完成模拟 release 后、写 tombstone 前暂停并被父进程强杀，随后第二个 child 重试并持久化 released，第三个 child 验证已落 tombstone 的代次不会重复执行。副作用计数为 2。`chast-host`、`chast-service`、`chast-host-parity-test`、`chast-portable-helper-lifecycle-test` 构建通过，高完整性 `--portable-helper-authorization` 通过，授权与生命周期 CTest 各重复 5 次 **10/10 PASS**。这不是正式 cleanup-worker/Named Pipe/桌面 API/SYSTEM/SCM/ProgramData 验收。仍按当前计划先解决键鼠物理来源冲突（generation-scoped virtual HID；WDK、签名和隔离验收环境是前置），产品 grant 保持 fail-closed。

2026-11-27 H01 released journal 生命周期回收：helper 最终 staging teardown 现在先枚举实例 journal artifacts 并严格解析 scope；所有已安装记录必须是空的 `released` tombstone 才能批量清理 `.dat/.tmp/.lck`，pending、损坏、未知 scope/artifact 或仍被打开的 lock 都会保留整个实例。回归在受保护隔离目录验证 possible 状态阻止全部删除、全部 tombstone 后回收成功及未知 artifact 拒绝。Release host/service/parity/lifecycle-test 构建通过；授权/生命周期 CTest 各重复 5 次 **10/10 PASS**；高完整性 `--portable-helper-authorization` 直接执行通过，`git diff --check` 通过。该验证不是 SCM/ProgramData 实机验收。下一项仍是厘清同一 VK/鼠标按钮与本机物理状态的可靠隔离方案，再进行签名管理员 cleanup worker 与异常退出验收。

2026-11-27 H01 物理输入冲突实现前置核查：键鼠 release 仍依赖全局 `SendInput`，tag 不能区分远端与物理同 VK/按钮的当前状态；没有源码级 raw-input monitor 能单独覆盖 worker crash 后的 cleaner。仓库没有 HID driver/backend。当前 SDK 含 `shared/vhf.h` 声明，但缺少 WDK `km/wdf.h`、`vhfkm.lib`、Windows Driver build props 和驱动目标，不能据此编译 VHF/KMDF 驱动。将此工作项具体化为 source-isolated 的 generation-scoped virtual HID keyboard/mouse backend；WDK、实际发布者签名和隔离机器验收未具备前，产品键鼠 grant 继续关闭，不把进程内 monitor 作为安全关闭条件。

2026-11-27 H01 cleanup tombstone failure/ACK-loss 测试：isolated protected journal 测试注入旧 record handle 阻止原子 tombstone 替换；executor 成功但 MarkReleased 持久化失败后 possible state 保留，重试会重复执行清理并写入 tombstone；再恢复已释放 tombstone 验证 ACK 丢失后的幂等处理不会重执行。Release parity test 重建，高完整性 authorization 套件通过；授权与生命周期 CTest 各重复 5 次 **10/10 PASS**。这不是实际 child kill/pipe ACK drop/desktop effect 测试，仍待签名隔离管理员环境验证。

2026-11-27 H01 cleanup-worker 接线：同一暂存映像现有独立 cleanup-worker mode，端点及请求按 instance/connection/session/generation 绑定；child 双向验证 peer/process identity，从受保护 journal 读取 possible 状态，按记录 desktop 释放并在成功后写 tombstone。正式 service 已配置 cleanup callback，失败时 journal 保留、Stop 前重试。物理输入冲突尚未解决，产品的显式策略门禁默认关闭键鼠 grant。Release host/parity/service/lifecycle-test 构建通过；授权与生命周期 CTest 各重复 5 次，**10/10 PASS**。该证据来自隔离多进程/fake cleanup callback，不包含真实签名 SYSTEM cleanup child、SCM/ProgramData 或桌面 API。下一步解决物理 VK/鼠标状态冲突，再做签名隔离管理员 cleanup-worker 实机及强杀/丢 ACK/重启恢复验收；在这之前继续 fail-closed。

2026-11-27 H01 cleanup core（接线前阶段记录）：journal v3 现将可能仍按下的输入绑定到 WinSta0 desktop name；带 observer 的 live injector 在没有可读的当前输入桌面，或仍有旧 desktop held state 时 fail-closed。新增从 `possible` 状态生成 exact release plan（仅本项目 tagged keyboard keys、已记录鼠标按钮、touch IDs、pen），以及 executor-success 后才可 MarkReleased 的 retryable journal transaction。Release host/service/parity 目标构建通过，`protocol`、`keyboard`、`portable_helper_lifecycle`、`portable_helper_authorization` 重复 5 轮 **20/20 PASS**；高完整性 `--portable-helper-authorization` 实际执行受保护 store 回归通过。尚未实现独立 authenticated target-session process、helper emergency-release 生产接线、released journal 生命周期删除或真实桌面/崩溃窗口验收；`dwExtraInfo` tag 不能单独区分相同 VK 的本机物理按键，因此该冲突策略也待设计。产品键鼠 grant 继续 fail-closed。下一项按[H01 专题计划](h01-portable-elevated-helper-design-2026-11-25.md)接入独立 cleanup worker 和身份验证，再验证崩溃重试闭环。

2026-11-27 H01 输入释放 journal（当时阶段记录）：受保护、逐 instance/connection/session/generation 的 Windows 原子文件后端及 worker `InputSideEffectObserver` 接线已实现；v2 released tombstone 防止已有 generation 被重置。Release host/service/parity/keyboard 目标构建通过；`protocol`、`keyboard`、`portable_helper_lifecycle`、`portable_helper_authorization` 重复 5 轮 **20/20 PASS**，高完整性 `--portable-helper-authorization` 套件也通过并实际运行文件后端回归。独立认证的目标 session 清理执行端和生命周期删除接线仍未实现，产品键鼠 grant 继续 fail-closed。详见[H01 专题记录](h01-portable-elevated-helper-design-2026-11-25.md)及[当前状态入口](current-status-and-next-plan-2026-11-17.md)。

2026-11-26 H01 更新：父端 helper worker RPC caller 已补齐隔离测试；`BeginGrant` 现在要求一次性、短时效本机批准 receipt，批准 gate 限定创建线程并要求实时连接/session/权限复核。Release host/service/lifecycle/parity 目标构建成功，H01 授权 CTest 连续 5 次通过、生命周期 CTest 1/1 通过。该审批 gate 与 caller 尚未接入 `host/main.cc` 实际 UI/UAC/staging/SCM；签名 pin 和产品输入释放 provider 未配置，故仍无产品可用提权会话。真实 SYSTEM/SCM/ProgramData/安全桌面验收继续待做，详见[H01 设计与实现状态](h01-portable-elevated-helper-design-2026-11-25.md)及[当前状态入口](current-status-and-next-plan-2026-11-17.md)。

2026-11-25 C03/H01：在当前源码重建 Windows Release Flutter 查看端，并以生命周期 trace 完成 20/20 个标签关闭/重连 loopback 周期；每轮兄弟连接保持 active，末尾精确断开通过。轨迹未出现窗口集归零或主消息泵 quit/error。11-22 历史进程退出根因仍未复现/定位，C03 保持未解决风险。H01 的 framing、受限 Named Pipe、生命周期协调器、独立 service entry、staging/copy、恢复记录及正式 `chast-service` 后台恢复扫描代码均已实现并有注入式/隔离回归；不过发布签名证书正向验证、授权/UAC 接线、真实 SCM/ProgramData 管理员验收、worker-ready/capability、桌面 worker 及安全桌面验收仍待。Win32 adapter 未在本轮调用，未安装服务或修改 SCM/ProgramData/防火墙。帧序号和消息类型不授予桌面操作权限。详见[H01 设计与实现状态](h01-portable-elevated-helper-design-2026-11-25.md)。

2026-11-25 H01 恢复推进：持久恢复记录和受保护实例扫描现已实现，正式 `chast-service` 在进入运行态后启动后台扫描线程；取消检查确保服务停止后不再启动下一实例清理，主服务等待当前操作退出并记录扫描汇总。Release `chast-service` 与生命周期回归构建通过，`portable_helper_lifecycle` 1/1 PASS，SCM 外 `--run` 按预期拒绝，未安装服务或触碰 ProgramData。该接线尚缺真实已安装 SCM 服务启动、孤儿记录清理和 STOP_PENDING 行为验收；H01 的 UAC 授权、worker-ready/capability、桌面桥接及安全桌面仍未完成。更新后的功能状态与下一步见[当前状态入口](current-status-and-next-plan-2026-11-17.md)和[H01 设计记录](h01-portable-elevated-helper-design-2026-11-25.md)。

2026-11-24 C03 失败注入与压力记录：真实 Flutter Release loopback 对无效 HWND 调用 `PostMessageW` 并验证回滚保持标签数、选中项和可见窗口不变，随后 3 轮关闭/重连通过；同日另有 20 轮关闭/重连、兄弟连接持续 active 及末尾精确断开通过。两者均未模拟消息队列满载，也未定位 11-22 历史进程退出根因。详细证据见[审核修复记录](review-fixes-2026-11-21.md)及[多会话标签设计](multi-session-tabs-design-2026-11-21.md)。

2026-11-24 验收环境检查：系统只枚举到一台 `IDV10bit2K144` 显示设备（是否物理显示器未确认）；音频端点为 ToDesk 虚拟扬声器/麦克风及显示器音频，未枚举到独立物理麦克风。物理多屏/热插拔、真实麦克风/AEC 仍无法在本机验收；未打开麦克风或改变显示拓扑，双机验收继续后置。

2026-11-23 H03：服务端设备签名分片 API、Go tunnel 默认关闭的持久上传 worker、C++ MP4 封口事件、原子任务清单 producer 和 opt-in/队列参数接线均已实现。Release CTest 4/4、Go agent/tunnel 包测试、vet、C++→Go E2E 及编译态 `chast-tunnel.exe host` 经 C++ `TunnelProcess` 父进程启动后的私有 CA/CONNECT 代理、强杀回收和 offset 续传均通过；完整 E2E 连续两轮通过。另以真实便携 `chast-host`、合成 VP9 桌面和真实 tunnel 完成本机封口上传整链，owner、SHA-256 和源文件保留断言通过。验收夹具已按构建目录 Low Integrity 标签调整为由 C++ producer 创建，未放宽权限。正式服务/部署、ProgramData/防火墙和双机验证后置。上传成功不删除本地 MP4。详见 [H03 主机归档设计](host-recording-archive-design-2026-11-23.md)。

本次归档前已提交基线为 `688638a`（2026-11-21 07:41:13 +08:00）。2026-11-23 快照包含后续源码、测试和状态文档；提交/推送结果以该日 Git 历史为准。下文的 `172516e`、分支比较、早期归档说明及音频样本均保留为 2026-11-20 历史记录。

2026-11-23 归档范围与状态：提交本轮工作区内的产品源码、自动化测试/验收脚本、设计和开发记录；排除 `.obj`、测试 `.exe`、`test-temp/` 等本机生成物。实现与本机证据已覆盖 C03/C04/H03，并修复 O01 活动连接快照跨 Store 的旧写入覆盖风险。C03 无效 HWND 下的真实 Win32 失败注入已于 2026-11-24 验收通过，但历史进程退出根因、C04 物理设备/真实应用互通、H03 正式服务/生产/双机、W1 外部验收，以及 W3/W4/W5 均未因此关闭。既有测试证据按原日志范围引用，本次归档不声称重新执行了全量发布门禁。

2026-11-23 C03 补充：`PostMessage(WM_CLOSE)` 失败且 HWND 仍有效时恢复标签关闭前状态，模型 CTest 1/1、Flutter Windows runner Release 构建通过；正常关闭/重连另追加 3 轮 Release loopback 并通过，累计 49 轮。2026-11-24 又通过无效 HWND 的真实 `PostMessageW` 失败注入及 host 回滚验收，随后同进程 3 轮关闭/重连和精确断开通过。该结果不解释 12:05/12:08 历史查看端进程退出，原因仍未定位，不据此宣布整体关闭问题修复。物理键盘/IME 仍待实机验收，快捷键 Release loopback 与最终 Flutter 870/870 证据见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 C04 SVG 已补：剪贴板协议更新到 v78 / CCLB v5，增加安全校验和 16 MiB 单项上限；128 MiB 合法组合预算获协议测试覆盖。双向 QUIC 回环、恶意/超限容器拒绝及隔离私有剪贴板 SVG 读写通过；本机探针首次揭露的“坏 SVG 连带丢弃文本/PNG”问题已修复并通过回归。其后可配置快捷键也已实现，并通过 Flutter、原生键盘 CTest 和 Windows Release 构建；物理键盘/IME 与真实应用剪贴板验收仍待。C03 关闭冲突、双机后置及其它硬件验收保持独立待办，详情见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 20:00 C04 已补边缘滚动与滚动样式：滚动条/自动/边缘三种模式，边缘范围 20–150 px，按设备连接目标保存；平移后鼠标坐标重映射，普通滚轮仍独占路由到远端。Flutter 861/861、analyze、Windows Release 通过。此前已实现剪贴板按键发送入口及触控板平移倍率。物理触控板/触摸设备和真实输入验收仍待。详见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 H03 查看端延伸已完成本机验证：真实 Flutter Windows 进程 PATCH 挂起时强杀，重启后通过 HTTP 恢复 offset、以 SHA-256 校验归档并清空队列；未封口 MP4 首页提示及永久保留/不自动删除策略已实现。全量 Flutter 848/848、analyze、Windows Release 通过。原 H03 主机入站集中归档、生产服务端/双机验收仍未关闭；标签关闭导致进程退出的冲突继续保留，按约束不改关闭路径。

2026-11-22 11:53：`-Flutter -MultiWindowDisplay` Release loopback PASS。合成 host 的两个逻辑显示器共用只读本机捕获；A/B 显示窗口随标签切换，断开 A 后仅 A 子窗口关闭，A 进入 ended，B 窗口和标签存活且继续解码（1→115）。证据见[审核修复记录](review-fixes-2026-11-21.md)。因此当前已覆盖合成拓扑的父子窗口生命周期；物理多屏、DPI/旋转/全屏/热插拔、双机仍待。选中标签关闭计数归零冲突保持独立，按约束未改关闭流程。

2026-11-22 11:10 更新：真实 Flutter 双标签 Release loopback 中，合成音频逐标签静音隔离及 C01 保活 lease 首断保留兄弟/末断归零均通过；结束会话后立即释放该页 lease 的修复已实现。Flutter 全量测试 829/829、`flutter analyze --no-pub` 无问题，双标签原生进程验收 PASS。物理电源行为、真实音频设备/语音与混合 DPI 仍待验收；选中标签关闭计数归零问题仍有冲突证据，不改关闭路径。详细证据见 [审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 10:20：专用双标签 Release loopback 连续 3/3 验证真实 Flutter 音量按钮只切换选中会话的静音状态，隐藏/切回保持状态且两路持续解码；主机合成静音 Opus，测试播放器丢弃输出，不触碰物理音频设备。最近证据见 `build/native-runtime-20261122-102955-c7e0c926/flutter-tab-audio-3261278057/`。标签关闭结果冲突仍保留且不改关闭逻辑；接下来补显示子窗口归属、多会话防休眠释放、显示场景，以及真实语音/设备验收（依环境）。

2026-11-22 后续：新增显示子窗口归属 Win32 回归，并修正结束会话后迟到 `setKeepAwake(true)` 仍能获取电源租约的竞态；相关 CTest 6/6、Flutter Windows Release 构建通过。随后双 Flutter 标签 Release loopback 已证明首会话断开释放一个 lease 且兄弟仍持有、末会话断开后全部释放；物理电源行为和真实多显示器父子窗口关闭/切换仍待完成；标签关闭冲突不改。

2026-11-22：C03 已通过同进程双标签切换/可见性、切换输入释放/后台拒绝，以及 B 隐藏时持续收帧、解码、发布、录制的 Release loopback；最新 08:31 证据见审核修复记录。选中标签关闭 07:07 曾通过，但 07:59/08:04 后续两轮观察到标签/窗口数归零，关闭结果冲突，需复现定位；逐标签媒体 UI/音频隔离、关闭失败回滚和显示子窗口归属仍待验收。录制撤权提示完成旧 native 原因分类/本地化及 widget 回归，Flutter 全量 828/828、analyze 和本地化测试通过；尚无新的 Release 实际弹窗采证。

2026-11-22 08:59：Release loopback 新增逐标签录制状态断言通过：A streaming 且不录制，B streaming 且录制，B 隐藏期间样本 215→271；输入释放/隐藏拒绝同轮通过。录制按钮 UI、音频播放/静音隔离仍待验收；关闭计数归零问题按用户约束单独保留，当前不修改关闭逻辑。证据见 [审核修复记录](review-fixes-2026-11-21.md)。

2026-11-21 21:30：新增录制撤权和 Flutter 子窗口初始化崩溃修复，定向回归及最终 Release 构建通过；真实子窗口已验证创建、开始录制、撤权停止、恢复权限不自动重启及关闭。录制撤权提示仍待完善，设备/完整重连/独立第二连接 UI/多会话防休眠释放与物理显示验收仍缺。多连接标签页只有设计，H03 队列重启恢复和 C04 尚未实现。详见审核修复记录。

2026-11-21 20:50：最新候选 C 的干净/弱网各五轮组合门禁 10/10 PASS，AVC 重配置/解码三场景和受影响原生/Flutter 定向检查通过。当前转入真实 Flutter 子窗口媒体操作及可用显示场景，随后多连接标签页、H03 重启恢复和 C04；C03 整体、W1 外部验收及后续 W3/W4 尚未完成。历史失败与环境缺口继续保留。

## 1. 2026-11-20 分支选择依据（历史）

本轮通过 `socks5://127.0.0.1:10808` 执行 `git fetch origin --prune` 后核对提交图，并执行 `git switch codex/complete-workspace-2026-11-20` 确认当前分支。核对时本地 HEAD 与同名远端分支差异为 `0/0`。

| 分支 | 远端提交 | 与当前开发基线的关系 | 用途 |
| --- | --- | --- | --- |
| `main` | `a609e44` | 当前基线完整包含其历史，并领先 29 个提交 | 保留主线基线 |
| `codex/s03-role-console-2026-11-17` | `4ffb103` | 完整包含在当前基线中；当前基线另有 1 个提交 | 保留 S03/W1 阶段记录 |
| `codex/complete-workspace-2026-11-20` | `172516e` | 当前 HEAD；包含上述两个分支的全部提交 | 后续开发使用 |

相对 S03 分支，当前开发基线新增改动涉及 91 个文件，包含 SCM 安装/卸载与防火墙事务加固、机器状态 ACL、C01 会话防休眠、C02 视图/光标缩放、C03 显示窗口布局恢复及会话隔离回归。因此选择此分支承接后续工作。提交数量仅用于核对包含关系，完成度判断同时参考实现范围和验收记录。

核对时已有 9 个跟踪文件存在未提交改动，主要为显示子窗口媒体控制、对应 Flutter 回归、真实麦克风探针及三个状态文档。未跟踪文件也已保留。上述改动不属于远端 `172516e`，应在整理、复核后单独形成后续提交。

## 2. 2026-11-20 阶段判断（历史）

- W1：S01/S02/S03 已形成必要代码闭环，已有数据库、浏览器及原生进程的本机证据；双机、正式 SCM/worker、安全桌面、真实防火墙写入回滚及生产数据库验收仍未关闭。
- W2：C01/C02 已有实现和本机自动化证据；C03 已覆盖窗口布局、连接/录制隔离及显示子窗口媒体控制，仍有音频质量、真实显示场景和多连接标签页缺口。
- 当时的显示子窗口媒体控制仅有 Flutter 实现与 widget 证据；2026-11-21 审核发现原生桥仍拦截对应方法，本地修复已补准入和旧会话拒绝回归。停止自动录制回归和查看端自适应播放已有实现；真实麦克风探针仅复用已有当前用户证书，缺少身份时拒绝运行，不创建证书或密钥。此前探针前后证书集合一致。
- 默认弱网同版本各 5 轮：固定缓冲空转中位数 8350 ms、丢弃中位数 830 块；自适应缓冲降至 3840 ms、369 块，但有 1/5 轮视频最大帧龄 3048 ms 超过 3000 ms 门槛。音频连续性与弱网视频门禁仍未关闭。逐轮数据见 W1 状态文档。
- 本次归档重新执行 Flutter、Go、管理台与受影响原生门禁，最终结果见 W1 状态文档“本次归档验证”。较早测试数字仍保留原证据范围。
- 继续沿用既有安排：双机验收后置，本机可独立完成的 W2 工作继续推进；W1 保留发布验收待办。

具体完成状态以 [当前状态入口](current-status-and-next-plan-2026-11-17.md) 和各项原始验收记录为准。

## 3. 接下来按顺序执行

| 顺序 | 工作包 | 具体动作 | 交付与退出条件 |
| --- | --- | --- | --- |
| 1 | 归档当前实现 | 2026-11-23 已将源码、测试、验收脚本及开发记录作为当前快照归档；本机 `.obj`/`.exe` 与 `test-temp/` 排除 | 本次归档不等于全量门禁重跑；未关闭项和验证边界见状态入口及审核记录 |
| 2 | C03 音频连续性修复（组合门禁已通过） | 候选 C 固定二进制干净/弱网各五轮 10/10 PASS；AVC 重配置/解码三场景通过，历史失败证据保留 | 既有音频预算、稳定链路 skew 和 3000 ms 弱网帧龄门槛未放宽；画质、真实音质与设备体验继续在下一工作包验收 |
| 3 | C03 真实音频与窗口控制验收 | 已通过合成音频下的真实 Flutter 双标签静音路由及 C01 首断/末断 lease loopback；继续使用可控语音和下行播放参考验证麦克风可懂度、AEC/降噪及设备切换，并检查真实多窗口撤权、关闭、重连和子窗口归属 | 剩余真实音频及物理设备证据；同一连接的子窗口状态一致，独立连接互不误操作；物理电源行为和真实窗口生命周期有明确结果 |
| 4 | C02/C03 显示场景收口 | 按可用硬件验证混合 DPI、旋转、全屏、多屏布局与物理显示器热插拔；核对视图、光标、输入坐标及离屏窗口恢复 | 每个场景记录设备/DPI、步骤和结果；硬件缺失项明确保留待验收；几何单测和模拟 DPR 不替代物理场景 |
| 5 | C03 多连接标签页 | 基础容器、新建、切换、输入隔离、兄弟媒体连续性、逐标签录制/静音 UI、音频隔离、C01 lease 释放及合成双逻辑显示器父子窗口生命周期已有 loopback 证据；正常关闭及无效 HWND 下真实 `PostMessageW` 失败回滚均有 Release loopback 验收 | 12:05/12:08 历史查看端进程退出需继续定位；本次未模拟消息队列满载；物理显示、电源及双机/长时验收依环境后置 |
| 6 | H03 主机整链验收与 C04 输入体验 | H03 查看端恢复及主机归档本机整链均已实现；Release tunnel 子进程已由 C++ `TunnelProcess` 拉起并通过私有 CA/CONNECT 代理、父进程强杀回收和 offset 恢复 E2E；2026-11-23 又以真实便携 `chast-host`、合成 VP9 桌面和真实 tunnel 完成封口上传、owner/SHA-256/源文件保留验收。C04 剪贴板按键发送、触控板、SVG、边缘滚动/样式及可配置快捷键实现和自动化已完成 | H03 生产服务/ProgramData/防火墙、双机验收后置，主机源 MP4 不自动删除；C04 真实剪贴板应用互通、键盘/IME 和物理触控板验收仍待 |

工作包 2 的组合门禁及双标签 loopback 音频/租约、合成双逻辑显示器父子窗口生命周期均已有通过证据；工作包 3/4 的真实音频、历史窗口关闭退出根因和物理显示场景仍未全部关闭。无效 HWND 下真实 `PostMessageW` 失败回滚已于 2026-11-24 验收通过，但标签关闭后查看端进程退出的历史来源仍未定位，也未模拟消息队列满载。H03 查看端强杀重启及未封口文件保留策略、主机归档从真实 `chast-host` 封口到 tunnel/服务端的隔离 loopback 整链均已通过。下一步按可用设备补物理显示/音频场景；正式服务部署、ProgramData/防火墙、双机和 W1 外部验收仍未关闭。2026-11-23 源码快照已归档；本机生成物排除。前序发现账号隔离、越权、数据丢失、错误断开或核心连接阻断问题时，先修复并回归。

### 音频复测当前结论

早期两个单次样本如下，保留为历史起点；后续已有多轮数据，不再据此安排首次复测：

| 场景 | 时长 | starved | dropped | underruns | A/V skew |
| --- | --- | --- | --- | --- | --- |
| 干净链路 | 25 秒 | 400 ms | 42 | 2 | +22.8 ms |
| 默认弱网 | 25 秒 | 8170 ms | 812 | 2 | +18.0 ms |

同版本默认弱网 5 轮中，固定缓冲空转范围 8070–9200 ms，自适应为 2980–4510 ms；自适应组整体验证 4/5 通过。1200 kbps 起始视频码率对照空转中位数 1670 ms、丢弃中位数 152 块，5/5 整体验证通过，但伴随码率/帧率取舍，不修改产品默认 3000 kbps。可靠音频流空转中位数 7200 ms、丢弃中位数 709 块，未优于数据报。保留已学缓冲余量的实验效果较差且出现视频帧龄失败，已从源码撤回。

早期干净链路自适应 5 轮空转中位数 430 ms（380–520 ms），来自另一二进制，不能充当上述同版本交替对照。稳定链路既有 skew 目标仍为 ±40 ms；播放连续性尚未建立可失败门禁。真实 USB 麦克风信号 RMS 0.4、峰值 27 且缺下行参考，语音可懂度和 AEC/降噪效果继续待验。

## 4. 环境具备后恢复的 W1 验收

这些是发布条件，继续登记；双机后置不等于取消验收：

1. 在可回滚 Windows 测试环境验证正式 SCM 安装/卸载、worker、服务身份、异常恢复、安全桌面、Firewall COM 写入及失败回滚；补齐此前因 `SeRestorePrivilege` 不可用而 Skipped 的 owner 集成路径。
2. 使用第二台 Windows 验证实际键鼠、跨机文件传输、精确断开后非目标持续可用、真实断网/重连、NAT、IPv6 和代理链路。
3. 完成生产 PostgreSQL TLS/权限、整库备份恢复、故障切换及全管理台辅助技术验收。

W3 的加密密码同步/共享地址簿/治理能力，以及 W4 的多实例与生产部署继续沿用 [原执行方案](rustdesk-execution-plan-2026-11-16.md)，不在本轮提前扩大实现范围。

## 5. 验证与提交规则

- 每个实现切片运行受影响的定向回归、静态检查和必要构建；提交前按影响范围补完整 Flutter、Go、管理台及原生门禁。
- 分开记录“实现”“自动化”“本机实测”“双机/生产”证据；失败和 Skipped 单列。
- 较早归档已提交、推送到 `codex/complete-workspace-2026-11-20`；2026-11-23 快照按源码/测试/文档归档并排除本机生成物。GitHub 同步使用指定 SOCKS5 代理，并在推送后核对远端 HEAD；未重跑全量发布门禁时须明确记录。

## 6. 关联记录

- [当前完成状态与后续计划](current-status-and-next-plan-2026-11-17.md)
- [功能状态与阶段路线](feature-status-and-plan-2026-11-16.md)
- [W1 验收明细及 C03 最新证据](w1-status-and-next-plan-2026-11-19.md)
- [RustDesk 对照审核执行方案](rustdesk-execution-plan-2026-11-16.md)
