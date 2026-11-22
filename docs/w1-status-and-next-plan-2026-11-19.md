# W1 当前状态、完成标记与后续计划

日期：2026-11-22

分支：`codex/complete-workspace-2026-11-20`

当前已提交基线：`688638a`（2026-11-21 07:41:13 +08:00）。审核后的本地修复、验证结果和剩余问题见 [2026-11-21 分支审核问题修复](review-fixes-2026-11-21.md)，尚未提交、推送。下文 `fbac887` 备份基线及 `172516e` 开发基线用于追溯历史；中间实验的失败、Skipped 和验收边界继续保留。

适用范围：Windows 单机验收、W1 企业管理主干收尾。本文是当前 W1 状态入口；历史文档中的中间结论如与本文冲突，以本文和对应原始证据为准。

### 2026-11-22 C03 多连接标签页基础验收

真实 Release Flutter loopback 建立两条会话并切换标签 `1 -> 0 -> 1`；每次原生状态均确认只有一个会话子窗口可见。08:31 专用模式继续证明切换输入释放/隐藏输入拒绝，以及 B 隐藏 1,867 ms 时收包/解码/发布各 253→309、录制样本 220→276，状态仍为 streaming/recording；证据：`build/native-runtime-20261122-083058-21a139b0/run-1332851354/`。选中关闭在 07:07 曾通过，但 07:59/08:04 两次后续验收出现标签/窗口计数归零，当前关闭结果冲突，需复现定位；逐标签媒体隔离及显示子窗口生命周期仍未完成。W1 的 SCM/防火墙/双机/PostgreSQL 生产验收不受此结果影响，仍未完成。同期撤权提示分类/本地化 widget 回归、Flutter 全量 828/828、analyze 和本地化回归通过；Release 实际提示显示未专项采证。

08:59 专用 Release loopback 新增并通过逐会话录制状态断言：A streaming 且 recording=false，B streaming 且 recording=true；B 隐藏 1,863 ms 期间收包/解码/发布各 248→304、录制样本 215→271。证据及边界见审核修复记录；录制按钮 UI、音频播放/静音隔离仍待验收，关闭问题按约束单独保留。

## 1. 总体结论

W1 尚未完成，当前不能宣称发布候选通过。已经完成的部分集中在 W1 企业管理主干、单机原生进程、管理台断开、Flutter 视频/文件链路、Flutter 窗口键鼠释放序列和临时 SYSTEM 服务监督验收；仍缺口主要是第二台 Windows 实机、跨网、正式 SCM 服务及生产环境。

当前证据等级分为三类：

- **已完成**：有自动回归或真实进程产物，能直接支撑对应结论。
- **单机已验证**：只在 loopback、本机模拟输入或临时服务身份下通过，不能替代双机、跨网或正式服务。
- **待验收**：代码或工具已经存在，但没有达到完成所需的真实环境证据。

### 2026-11-21 晚间审核修复状态

2026-11-21 21:30 补充：录制撤权不停止及 Flutter 子窗口初始化无障碍查询崩溃已在本地修复。录制新增回归修复前失败，修复后与会话/音视频隔离共 3/3 PASS；Flutter 定向 10/10、analyze、Release 构建及双 engine 初始化回归通过。真实隔离 Release 验证子窗口创建、录制开始、撤权停止、恢复权限不自动重启，以及关闭子窗口后主窗口存活；测试窗口和 fixture 已结束。撤权提示仍使用“录制失败”与英文原因，待完善。完整重连/独立第二连接 UI、设备/显示环境和多会话防休眠释放尚未验收。失败记录、版本哈希及范围见 [审核修复记录](review-fixes-2026-11-21.md)。C03 整体、标签页、H03/C04 和 W1 外部验收仍未完成，所有本地修复尚未提交、推送。

2026-11-21 20:50 更新：持续迟到播放重锚、拥塞降码率单调性及 AVC 隐含最低码率修复后，候选 C 固定二进制干净/弱网各五轮组合门禁 10/10 PASS。干净 skew +7.5～+33.6 ms；弱网最大视频帧龄 962～1801 ms、最大设备断档 140～305 ms，仍不等同于无断档或真实音质达标。AVC 三种码率/FPS 重配置均按请求回报且完整解码；原生 9 项单测、4 项集成及 Flutter 6 项定向测试、Release 构建通过。哈希、逐轮指标和原始日志见 [审核修复记录](review-fixes-2026-11-21.md)。C03 的实际媒体/显示场景与多连接标签页仍未完成。下段及后续候选 B 五轮、三次音频失败均为保留的历史证据，不能用通过结果覆盖。

显示子窗口媒体准入、补画质帧丢弃后的 IDR 重同步、QUIC 工具失败退出时的音频线程 join，以及 Num Lock 布局映射已在本地修复；原生定向 8/8 PASS，最终 Flutter Windows Release 构建 PASS。主机新增独立音频转发线程，播放期限计入设备 padding，到达相位使用固定参考。最终候选 B 的干净/弱网各五轮音频预算均通过，十轮主机采集/发送丢弃为 0；干净 skew +6.3～+37.5 ms，满足原 ±40 ms 目标。整体仅 9/10 通过：弱网第 1 轮视频最大帧龄 3347 ms 超过 3000 ms。C03 和发布门槛仍未完成，完整数据及后续计划见 [修复记录](review-fixes-2026-11-21.md)。旧样本和未验收项保留，不以新音频结果覆盖视频失败或真实听感缺口。

### 2026-11-21 早间备份状态（修复前历史）

| 项目 | 当前状态 | 后续要求 |
| --- | --- | --- |
| C03 低 FPS 下的音视频处理隔离 | 已实现，单机回归通过 | 查看端音频由独立工作线程接收/解码；5 FPS、视频消费停顿、断开与重连由 `viewer_audio_video_isolation` 覆盖，双机仍待验。 |
| 音频重排、播放恢复及逐包诊断 | 已实现并有定向回归 | 保留 20 ms 重排等待与查看端迟到容忍；继续用提交、成帧、接收、播放事件定位长断档。 |
| 查看端系统声音重锚时保留 PCM | 已启用，完成多轮单机模拟 | 最近干净/弱网各五轮已完成；弱网 5/5 通过实验预算，干净链路整体验证 4/5，通过边界详见本节后文。 |
| 主机音频丢弃分项 | 已实现，Release 构建及本次五项 CTest 通过 | 新增采集/发送丢弃分项及前三次发送丢弃原因；新统计后的专门复测曾中断，旧样本的 3 个主机丢弃仍待归因。 |
| C03 音频验收 | **未完成** | 实验预算不是产品标准；干净链路 skew 尚未达到既有 ±40 ms 目标，真实听感、可懂语音、AEC/降噪与真实跨网未验收。 |
| W1 外部验收及其余 W2 | **未完成** | 正式 SCM/生产拓扑、双机、混合 DPI/真实多屏、物理热插拔仍待验；多连接标签页、H03 跨重启队列和 C04 后续实施。 |

本次提交前定向 CTest `audio`、`audio_dsp`、`audio_playout`、`viewer_audio_video_isolation`、`quic_weak_network` **5/5 通过，37.68 秒**。该结果不包含历史 `protocol` 锁键断言失败的修复或复验，也不代表完整发布门禁通过。后文按日期保留各轮实验的原始结论，当前状态以本表及最新记录为准。

## 2. 已完成项

| 项目 | 状态 | 证据与边界 |
| --- | --- | --- |
| S01 通用 Strategy | 已完成代码闭环，单机真实验证 | 服务端分配、签名投递、主机/查看端逐项回执、更新后下一会话采用新 revision、撤销后空策略已验证；证据 `build/native-runtime-20261118-222855-69bd0658/`。双机/跨网仍待验。 |
| S02 活动连接与精确断开 | 已完成代码闭环，单机真实验证 | 真实管理台页面按 connection ID 确认断开，目标会话产生签名 `confirmed_absent` 回执，非目标 Flutter 连接保持 active 且统计持续变化；边界仍为 loopback。 |
| S03 自定义 Admin Role | 已完成代码闭环，主要场景已验证 | 权限键、命名角色、global/group/object 绑定、权限上限、最后管理员保护、revision 并发保护已接通；SQLite/PostgreSQL 权限矩阵和主要浏览器场景已通过。 |
| F1–F4 审核缺陷 | 已完成代码修复 | 地址簿缓存覆盖、退出换号竞态、过期令牌录制归档、管理台换号刷新竞态已纳入回归；真实双客户端/慢网络/长录制仍有边界。 |
| R1–R5 S03 审核缺陷 | 已完成 | 账号提权、应急权限混用、设备删除权限错配、作用域入口/列表、机器令牌断开均已修复并有回归。 |
| Flutter 视频与文件链路 | 单机已完成 | 真实 Release Flutter 完成密码认证、首次指纹确认、HEVC/D3D11VA 收流；文件浏览器实际上传/下载 524325 字节并通过 SHA-256 核对。 |
| Flutter 双查看端与管理台断开 | 单机已完成 | 两个真实 Flutter 会话同时在线；管理台断开目标后，非目标连接保持 active，统计区在相邻样本中持续变化。 |
| 原生桌面流、输入映射与连接上限 | 单机已完成 | DXGI 1920×1080、MFX HEVC、D3D11VA 接收 39 帧；键盘回环探针通过；受管 3/4、本地 2/3 准入边界通过。 |
| 最低帧率下限 | 已确认 | `app/lib/widgets/remote_toolbar.dart` 的 `_minFps = 5`，本轮无需新增逻辑。 |
| 临时 SYSTEM 服务监督验收 | 单机已完成，范围有限 | SYSTEM 读取交互用户会话、登录用户身份子进程、UAC restricted token、重复轮询去重、异常退出 backoff、销毁监督不杀用户 manager、隐藏入口/无监听/不改普通用户设置等均 PASS；证据 `build/service-manager-startup-system.log`、`build/service-manager-entry-check.log`、`build/service-manager-entry-check.ps1`。 |
| Flutter 键鼠下行与释放 | 单机已验证 | 真实 Release Flutter 窗口发送鼠标和 A down；主机收到 `InputRelease`、排队 release barrier，并在 `release_all()` 中注入 A-up；物理清理 A-up 后无第二条远端 A 事件。仅 loopback + dry-run 输入验收，不代表双机或真实桌面注入。最新证据 `build/native-runtime-20261119-203601-9bc48695/run-1423269566/`。 |

## 3. 2026-11-19 Flutter 键鼠释放验收结果

执行：

```powershell
$env:PATH = 'D:\flutter\bin;' + $env:PATH
.\tools\test-native-runtime.ps1 -BuildDirectory build\root -Flutter -InputInjection
```

本轮真实产物：

```text
D:\ds-chcast\build\native-runtime-20261119-203601-9bc48695\run-1423269566\
```

验收结果：**PASS**（Go 用例 12.781 秒）。验证到控制开启/关闭状态、鼠标和 A-down 下行、主机 `input_release_received`、`release_held_queued`、`release_all_begin/end`；A-up 注入发生在 release 区间内、物理清理边界之前，边界之后没有新增远端 A 事件。轨迹索引为 A-up `11`、物理清理边界 `13`。

为避免真实 Flutter 验收写入受限的用户配置目录，测试模式现将访问票据私钥放进本次隔离 profile；正式运行仍使用系统 roaming profile。验证器兼容 Windows PowerShell 输出的 UTF-8 BOM JSON。修复前失败记录保留于 `build/native-runtime-20261119-001020-ef1059e1/run-2160080674/`。

2026-11-19 晚间复跑：显式加入 `D:\flutter\bin` 后，隔离 loopback 输入验收 PASS（Go 用例 12.065 秒）。A-up 索引 `11`，物理清理边界 `15`；host 收到 release barrier 并在 `release_all()` 内注入 A-up，边界后没有新增远端 A 事件。证据 `build/native-runtime-20261119-225354-3f006de1/run-657812140/`。首次启动因当前 PowerShell PATH 未包含 Flutter SDK 而未进入验收；补齐 PATH 后复跑成功。

## 4. 已确认的设计与代码事实

- 最低 5 FPS 已存在，无需重复新增。
- 关闭控制逻辑在 `app/lib/pages/session_page.dart` 的 `_toggleControl()` 与 `_releaseRemoteInput()`：先释放本地记录的 `_heldKeys`，再释放鼠标，最后调用 `ViewerBridge.releaseInput()`。
- 原生输入采集在 `app/windows/viewer/keyboard_capture.cc`；物理 `SendInput` 的 `dwExtraInfo` 为 0，带测试注入标记的事件会被忽略。
- 测试轨迹分别记录主机收到 `InputRelease`、`release_held()` 排队、`release_all()` 执行区间及 dry-run 注入的键鼠事件；生产调用不启用这些观察器。
- Flutter 输入验收启用隔离票据密钥目录；主机使用 `CHAST_NATIVE_TESTING=1`、`CHAST_NATIVE_INPUT_DRY_RUN=1` 和 `CHAST_NATIVE_INPUT_TRACE=...\input-events.jsonl`。

### 机器状态目录 ACL（代码已加固，尚未接入真实 ProgramData）

- 只读检查确认 `C:\ProgramData` 的 DACL 给 `BUILTIN\Users` 写权限，而 `C:\ProgramData\chast` 当前不存在。新增共享机器状态安全层：首次创建时直接带受保护 DACL；既有目录/文件仅迁移 SYSTEM/Administrators/当前提升安装账户拥有的对象，并用独占 no-follow 句柄替换旧 ACL；拒绝非可信 owner、reparse 和硬链接；失败时保留现场并关闭服务启动/设置读写。
- 机器目录仅给 Users 读取/遍历，不把此 ACE 继承给文件；机器 host.ini 与其他状态文件只允许 SYSTEM/Administrators 访问。虚拟显示器安装状态和日志继续保留 Users 只读；目录/状态读写已复用统一安全层。
- `chast-service --token` 要求提升的管理员 token，并只调用目录/文件 owner 与 DACL 的只读信任检查；缺失或不可信状态应拒绝且不能创建/修复机器状态。托盘提示和 M3.2b 文档已说明需从提升命令行读取。
- 临时目录 ACL 测试覆盖旧目录继承 Users/Creator Owner 写 ACL 迁移、内容保留、机器设置隔离、虚拟显示状态 Users 只读、可信 owner 文件 DACL 收紧，以及目录 junction/reparse 拒绝且目标不变。另验证只读信任检查对缺失目录返回不可信且不创建；受信目录下缺失配置文件按首次写入语义允许，但不创建文件。非可信 owner 拒绝分支尚未执行：此前夹具设置 `BUILTIN\Users` owner 返回 `ERROR_INVALID_OWNER (1307)`，启用 `SeRestorePrivilege` 返回 `ERROR_NOT_ALL_ASSIGNED (1300)`。该分支现拆为独立 CTest `machine_state_security_untrusted_owner`；无法启用权限时专项 Skipped，基础 ACL 测试单独报告结果。真实 SCM/COM 与升级前后行为仍待可回滚测试机验收。

## 5. 后续计划

### P0：Flutter 键鼠释放验收（单机已完成）

1. 已完成测试专用主机释放轨迹、Flutter 真实窗口开关验收、释放顺序断言和物理 A-up 无重复下行断言。
2. 当前证据限于本机 loopback 与 dry-run 注入；第二台 Windows 实机上的键鼠和真实输入注入仍列入 P2 外部验收。

### P1：SYSTEM 服务范围收口

1. 将现有临时 SYSTEM 服务证据登记为“临时服务监督验收通过”，明确不是正式 `chast-service.exe --install`、`--run`、`--worker`。
2. 本机代码级回滚和非破坏性回归已完成；正式 SCM 安装、服务身份、启动/停止、worker、安全桌面、锁屏和账号切换仍须在可回滚测试机验收。当前机器明确不执行这些变更。
3. 正式服务验收前，不以临时服务日志替代发布门槛。
4. 已加入 `%ProgramData%\chast` 安全创建/迁移与 `--token` 管理员门槛；后续在专用测试机验证正式安装、旧安装升级和真实 SCM 下的文件访问边界。

2026-11-20 ACL 与 token 只读路径复验：Release `chast-service`、`chast-machine-state-security-test` 构建通过（有既有 LNK4098/LNK4199 链接警告）。修正前执行 `chast-service --token` 时，命令误调用目录加固并尝试创建真实 `C:\ProgramData\chast`，返回 `ERROR_ACCESS_DENIED (5)`；随后只读确认目录仍不存在、无 chast 服务或产品进程。已改为只读信任检查；未在修正后再次针对真实 ProgramData 运行该命令。新增临时目录测试确认缺失目录不被信任且不创建，可信目录下的缺失设置文件不会被检查过程创建。最终 18 项 P1 定向 CTest 总耗时 189.47 秒，17 项通过、`machine_state_security` 1 项 Skipped。已覆盖的 ACL 路径包括旧继承 ACL 迁移、配置内容保留、Users 权限收紧、虚拟显示状态只读及 junction/reparse fail-closed；目录 junction 确认被拒绝且目标内容不变。非可信 owner 夹具设置 `BUILTIN\Users` owner 返回 1307；CTest 进程启用 `SeRestorePrivilege` 返回 `ERROR_NOT_ALL_ASSIGNED (1300)`，后续已拆为独立专项并确认该权限不可用。所有 ACL 样本均在 CTest 隔离临时目录；没有成功创建或修改真实 `%ProgramData%`，未执行服务安装/卸载、未写防火墙，也未做双机测试。

2026-11-20 owner 夹具隔离补强：将 `machine_state_security` 基础 ACL 覆盖与非可信 owner 专项拆分。Release 测试目标构建通过（LNK4199 delay-load 警告）；两项定向 CTest 中基础测试 PASS，owner 专项因当前进程令牌无法启用 `SeRestorePrivilege`（`ERROR_NOT_ALL_ASSIGNED`，1300）而单独 Skipped。专项只在 CTest 隔离临时目录准备 owner fixture；权限启用失败后没有实际改写 owner，RAII 仅在权限成功启用时负责恢复 token privilege。本次没有留下测试目录。`git -c core.whitespace=cr-at-eol diff --check` 通过；没有触碰真实 ProgramData、SCM 或防火墙。

2026-11-20 安装配置回滚审查：发现 `save_host_settings()` 可在原子替换配置文件后因最终 ACL 校验失败而返回 false；原安装流程仅在返回 true 后才登记 `FileSnapshot` 回滚，可能导致安装报告失败但新配置已落盘。现将快照恢复动作在保存前加入回滚栈，并增加模拟“写入已发生、保存随后失败”的 `service_install` 回归。Release `chast-service`、`chast-service-install-test`、`chast-file-snapshot-test` 构建通过；`service_install` 与 `file_snapshot` 定向 CTest 2/2 PASS。未执行 SCM 安装/卸载或防火墙写入。

2026-11-19 本机只读预检：Release `chast-service` 目标重建通过，`--help` 正常；`--run` 在 SCM 外按预期以 exit code 1 拒绝启动。`service_log`、`host_parity`、`host_security`、`host_settings`、`host_multi`、`host_i18n`、`lock_after_session`、`host_ticket_lease` CTest 共 8/8 通过（分两组耗时 129.86 与 59.75 秒）。检查时没有已注册的 chast 服务，也没有 `%ProgramData%\chast\host.ini`。正式 `--install` 尚未执行：安装会写机器设置、注册自动启动 SCM 服务并修改入站防火墙规则；当前尚未确认该机器是可回滚测试机。manager 管道、机器设置和临时 SYSTEM 服务证据都不替代产品 `chast-service.exe --install/--run/--worker` 生命周期验收。

2026-11-19 `host/service.cc` / `host/platform.cc` 安装卸载路径初审（只读；未执行 `--install`、`--uninstall`，未修改机器状态）。下列最初缺口已由后续实现记录更新；当前状态以本节末尾的跨阶段回滚和卸载保护记录为准：

- 初审版本会在加载配置时立即保存机器设置。后续改为安装时延迟持久化，并在保存前快照 `%ProgramData%\chast\host.ini`；安装失败时恢复原文件或移除新建文件。
- 初审版本只在防火墙阶段内部回滚。后续增加跨 SCM/机器配置/防火墙的回滚栈：新服务失败时停止并删除，已有服务失败时恢复原程序路径和启动类型；防火墙事务保留规则快照并报告原错误及回滚错误。fake backend 覆盖规则事务，但真实 COM `Rules.Add` 恢复仍未在专用测试机验收。
- 规则使用固定名称（`chast host`、`chast host tcp`、`chast tunnel`、`chast tunnel discovery`）。2026-11-19 后续修改已改用 Windows 防火墙 COM API 按名称查询：不存在的规则作为无需删除处理；删除前要求应用路径匹配当前产品程序，host 规则同时允许同目录的 `chast-service.exe` / `chast-host.exe`，tunnel 规则要求匹配 `chast-tunnel.exe`；归属不符时拒绝删除。`--uninstall` 在 SCM 明确返回服务不存在时仍调用规则清理，但不会删除指向其他程序的同名规则；卸载不会删除机器设置文件。SCM 权限不足或其他打开错误则会提前失败，不清理规则。
- 初审还发现子进程等待超时未区分处理，已由下方 `run_hidden_command()` 修正记录覆盖。2026-11-20 后规则查询、删除、快照恢复和创建均改为 Windows Firewall COM；通用 `run_hidden_command()` 保留给子进程测试，但不再参与防火墙事务。真实 SCM 生命周期与真实 COM 写入/恢复仍未验收。
- 以上实现与 fake backend 测试只证明代码路径，不代表真实 SCM/防火墙操作已复现。专用测试机仍须逐项验收安装成功、各失败阶段、服务缺失卸载、同名规则预置、配置文件保留、规则最终集合、服务身份和会话行为。

2026-11-19 本机子进程超时修正：新增 `run_hidden_command()`，等待超时后尝试终止并回收子进程；无法确认退出时返回终止/等待错误，正常超时且已回收时返回 `ERROR_TIMEOUT`。防火墙规则更新/卸载现在会传播子进程无法启动或等待失败。删除命令已退出但返回非零时仍被忽略，尚不能区分“规则不存在”和真实 netsh 错误。新增 `platform_process` CTest，以本测试程序自启子进程验证正常退出码和超时后子进程已结束。Release `chast-platform-process-test`、`chast-service`、`chast-host` 构建通过；本轮 P1 相关 `platform_process`、`service_log`、`host_parity`、`host_security`、`host_settings`、`host_multi`、`host_i18n`、`lock_after_session`、`host_ticket_lease` CTest 9/9 通过（190.54 秒）。该测试未调用 netsh、未修改防火墙、未注册/启动 SCM 服务；安装中途失败回滚、规则归属核验及真实 SCM 生命周期仍待处理/验收。

同日修正后的只读基线复核：SCM/WMI 无 `chast-host` 服务、无 `chast-service.exe` 进程、无 `%ProgramData%\chast\host.ini`；`netsh advfirewall firewall show rule` 对四个产品规则名均返回 “No rules match the specified criteria”。本机状态干净，但尚未确认它是可回滚的专用测试机，因此没有执行正式安装。

同日新增受保护的 SCM 验收入口 `tools/test-service-scm.ps1`。默认只读检查干净基线；真正执行必须同时指定 `-Execute -AllowMachineChanges`、使用提升的交互会话，并手工输入确认词。执行模式覆盖正式安装、SCM 路径与 LocalSystem 身份、交互会话 SYSTEM worker、停止/退出、重新启动/恢复、主机/tunnel 四条 UDP/TCP 防火墙规则的程序路径/协议/端口核对、卸载清理及机器设置保留；无论验收成功或失败都会尝试卸载并检查残留。PowerShell Parser 通过，默认只读预检 PASS；仅传 `-Execute` 时执行门禁拒绝，且复查机器基线未改变。未运行实际执行模式。锁屏、安全桌面、账号/RDP 切换及双机/跨网仍是人工验收项。

2026-11-19 用户确认当前机器不是可回滚的测试机，限定为“只做非破坏性测试”。因此本机禁止运行 `chast-service.exe --install/--uninstall` 或验收脚本 `-Execute`，也不修改防火墙。后续环境若未获用户重新授权，SCM 正式生命周期只能标为待验收。只读验证：`tools/test-service-scm.ps1` 默认预检 PASS；`chast-service.exe --help` exit 0；SCM 外调用 `--run` exit 1 并按预期报告必须由服务管理器启动；无机器配置时 `--token` exit 1 并报告配置不存在（未输出任何令牌）；`platform_process` CTest 1/1 PASS。以上命令未触发安装、卸载或机器配置/防火墙写入。

2026-11-19 防火墙规则归属保护：将规则存在性/程序路径查询与删除改用 Windows Firewall COM API。规则缺失时不调用删除；同名规则若不指向当前 host/service 产品程序或 tunnel 程序则返回冲突错误，不触碰该规则；COM 查询和删除错误向上传播。只读 PowerShell COM 探针确认四个产品规则名均缺失，返回 `HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)`（`0x80070002`），与代码的“无需删除”分支一致。增加纯逻辑 `firewall_rule` CTest，覆盖大小写不敏感路径匹配、host/service 同目录二进制兼容、异程序冲突和无应用路径规则拒绝。Release `chast-service`、`chast-host`、`chast-firewall-rule-test` 构建成功；P1 定向 CTest（`firewall_rule`、`platform_process`、`service_log`、`host_parity`、`host_security`、`host_settings`、`host_multi`、`host_i18n`、`lock_after_session`、`host_ticket_lease`）10/10 通过，196.79 秒；改动后核心测试复跑 2/2 通过。测试未调用防火墙写 API、未注册/启动/卸载 SCM 服务。安装阶段化回滚、真实 COM 防火墙修改/回滚验收和正式 SCM 生命周期仍未完成。

2026-11-19 防火墙阶段事务：新增 `firewall_transaction`，将规则集替换抽成可注入后端的事务编排：先验证所有固定名规则归属并快照，再移除旧规则、添加目标规则；任何操作失败时逆序恢复快照，并分别保留原操作错误和回滚错误。Windows 后端在同一 COM apartment 持有原 `INetFwRule` 对象，按需经 `Rules.Add` 恢复；安装启动和卸载清理共用此路径。新增 fake backend 用例覆盖成功替换、后续添加失败完整恢复、删除失败恢复、冲突预检零写入、回滚失败可见。`install_service()` 也将 `OpenSCManager(SC_MANAGER_CREATE_SERVICE)` 前移到设置/防火墙写入之前。Release `chast-service`、`chast-host`、两个防火墙测试目标构建成功；新增核心测试 3/3 通过；完整 P1 定向 CTest（含 `firewall_rule`、`firewall_transaction`、`platform_process` 等共 11 项）11/11 通过，194.62 秒。本机未调用防火墙写 API或 SCM 生命周期命令。仍缺机器配置回滚、SCM 创建/更新/启动阶段回滚，以及 COM 真机失败注入验收。

2026-11-19 跨阶段回滚补充及非破坏性复核：安装流程现在在首次写机器设置前保存文件快照；新建 SCM 服务失败时停止并删除服务，更新既有 SCM 服务失败时恢复原启动类型/程序路径；设置保存、服务状态和防火墙阶段通过回滚栈逆序协调。代码审阅确认句柄在回滚完成后才关闭；`FileSnapshot` 的已有文件恢复使用替换，原先不存在的文件在失败回滚时删除。新增 `file_snapshot` 和 `rollback_stack` 测试，分别覆盖配置文件原样恢复/移除及回滚逆序、首个错误保留、提交不执行清理动作。

2026-11-19 卸载停止保护：进一步审阅发现旧卸载路径忽略停止请求失败和 30 秒状态等待结果，仍会删除 SCM 项并清理规则。现改为复用 `stop_service_if_running()`；只有确认进入 `SERVICE_STOPPED` 才继续 `DeleteService` 和防火墙清理。停止失败或超时会返回错误并保留服务登记及防火墙状态。Release 构建与下方 P1 定向 CTest 通过；由于当前机器禁止 SCM 写入，此分支尚无真实服务停止失败/超时注入验收。

Release `chast-service`、`chast-host` 与相关测试目标构建通过。完整 P1 定向 CTest（`platform_process`、`firewall_rule`、`firewall_transaction`、`file_snapshot`、`rollback_stack`、`service_stop`、`service_log`、`host_parity`、`host_security`、`host_settings`、`host_multi`、`host_i18n`、`lock_after_session`、`host_ticket_lease`）14/14 PASS，195.00 秒。新增 `service_stop` 用 fake SCM 状态覆盖已停止、停止中、成功停止、状态查询错误、不可重试停止错误、瞬时控制错误和超时；该测试未创建或控制真实服务。`tools/test-service-scm.ps1` PowerShell Parser 与默认只读预检均 PASS；`chast-service --help` exit 0，SCM 外 `--run` exit 1 并报告必须由服务管理器启动，无机器配置时 `--token` exit 1 并报告配置不存在。验收前后均确认无 `chast-host` 服务/产品进程、无 `%ProgramData%\chast\host.ini`、无四条产品防火墙规则。测试只写入并清理临时目录中的快照样本；没有执行 `--install`、`--uninstall`、验收脚本 `-Execute`，也没有调用防火墙写接口。上述仅验证本地代码路径和模拟事务；真实 SCM 回滚、真实 COM 规则恢复以及锁屏/会话切换仍待获准的专用可回滚测试机验收。

2026-11-19 按计划非破坏性复验：Release `chast-service`、`chast-host` 和 P1 相关测试目标重建成功；14 项 P1 定向 CTest 全部通过，190.81 秒。`tools/test-service-scm.ps1` Parser 检查及默认只读预检通过，仍确认无 `chast-host` 服务/产品进程、无 `%ProgramData%\chast\host.ini`、无产品防火墙规则。本次构建输出 LNK4199 delay-load 及少数测试目标 LNK4098 CRT 配置警告，未导致失败。未执行服务安装/卸载、验收脚本 `-Execute`、防火墙写入或双机测试；因此 W1 外部验收仍未收口。按 2026-11-20 用户决定，双机验证后置；W1 未收口期间可继续推进 C01 的本机代码和自动化验证，但不把这视作 W1 验收完成。

2026-11-19 SCM 事务静态审查补强：发现既有服务处于运行/过渡状态时，安装可能改写服务程序路径或首次生成机器配置，随后把 `ERROR_SERVICE_ALREADY_RUNNING` 当作启动成功；现要求运行中的服务仅允许无路径/设置变化的幂等安装，路径或机器设置需要更新时先停服，过渡态与暂停态拒绝继续。新增纯逻辑 `service_install` 测试覆盖停止、运行幂等、运行中需更新及过渡/暂停状态。另将有服务时的卸载顺序改为先停止服务、事务性清除防火墙规则、最后删除 SCM 项；规则清除或服务删除失败时恢复规则，并在需要时尽力恢复此前运行/暂停状态，避免防火墙清理失败后服务已不可恢复地删除。新增 fake `firewall_transaction` 用例验证规则清空后可回滚恢复原集合。Release `chast-service`、`chast-host` 和相关测试目标构建通过；P1 定向 CTest 扩为 15 项，15/15 PASS，189.66 秒；验收脚本 Parser 与默认只读预检再次通过。未执行真实服务生命周期或防火墙写入；SCM 状态恢复和真实 COM 失败回滚仍待专用可回滚测试机验收。

2026-11-19 SCM 卸载编排回归补强：将停止、清规则、删服务及失败回滚顺序抽为可注入事务协调器，并把运行/暂停服务恢复状态机置于可 fake 测试的接口。新增 `service_uninstall` 用例覆盖停止失败不继续、规则清除失败不删服务、删除失败先恢复规则再恢复服务、成功提交，以及原运行/暂停状态重建。Release `chast-service`、`chast-host` 和测试目标构建通过；P1 定向 CTest 现为 16/16 PASS，188.36 秒。验收脚本 Parser 与默认只读预检 PASS，仍无产品服务/进程、机器配置或产品防火墙规则。测试未调用真实 SCM 或防火墙写接口；正式 SCM 与 COM 回滚仍待专用测试机验收。

2026-11-19 安装运行策略事务补强：安装现会先停止已有服务再更新服务路径、机器配置及防火墙，成功配置后才重启并等待 `SERVICE_RUNNING`；失败时逆序恢复新配置，并恢复原运行/暂停状态。此前被忽略的服务描述、延迟自动启动和失败重启策略更新现在纳入失败检查；既有服务安装前会快照三项 SCM 属性，失败时先恢复旧路径/启动类型和属性，再恢复原服务状态。新增 fake 用例验证运行中服务停止/失败恢复及运行策略中途失败后执行快照回滚。Release `chast-service`、`chast-host` 和测试目标构建通过；相关核心 CTest 3/3 通过，完整 P1 定向 CTest 16/16 PASS，189.21 秒。SCM 验收脚本 Parser 与默认只读预检通过，未安装/卸载服务或写入防火墙。真实 SCM `QueryServiceConfig2W` 快照恢复及失败注入仍待专用可回滚测试机验收。

2026-11-20 防火墙创建后端加固：移除提权安装路径对裸名 `netsh.exe` 子进程的依赖，改由已初始化的 Firewall COM apartment 创建 `INetFwRule` 并调用 `Rules.Add`；规则属性通过 COM 字符串/枚举设置，避免命令行解析与可执行文件搜索路径参与防火墙规则创建。Release `chast-host`、`chast-service` 及 firewall/service 测试目标构建通过；`platform_process`、`firewall_rule`、`firewall_transaction`、`file_snapshot`、`service_install`、`service_uninstall` CTest 6/6 PASS。测试未调用防火墙写 API；真实 COM `Rules.Add` 与失败恢复仍须在获准的可回滚测试机验证。

2026-11-20 防火墙与安装事务后完整 P1 复验：Release 构建 18 个 P1 测试目标通过（保留 LNK4098/LNK4199 链接警告）；CTest 18 项中 17 项 PASS，`machine_state_security_untrusted_owner` 因 `SeRestorePrivilege` 不可用（错误 1300）Skipped，总耗时 190.23 秒。`host_parity` 通过（124.30 秒）。安装/卸载覆盖仍为 fake SCM/事务，不代表真实服务或 COM 写入验收；测试未调用正式 `--install`/`--uninstall`，也没有写防火墙。

2026-11-20 owner 策略隔离测试：将 SYSTEM、Administrators、提升安装用户与 BUILTIN Users 的 owner 信任判定抽为内部纯策略函数，并接入生产 ACL 检查；新增 6 项合成 SID 断言，覆盖迁移与只读信任规则。Release `chast-service` 与 18 个 P1 测试目标重建通过；完整 P1 CTest 18 项中 17 项 PASS、owner NTFS 集成专项因错误 1300 Skipped，188.71 秒。该策略单测不替代真实对象 owner 验收；本轮未修改真实 ACL、ProgramData、SCM 或防火墙。

2026-11-20 Firewall COM 对象级回归：只读 smoke test 首次发现 `INetFwRule::put_LocalPorts` 在 `put_Protocol` 前调用会返回“Value does not fall within the expected range”。将生产规则属性配置抽为共享 helper，调整为先设置 UDP/TCP 协议再设置本地端口；用真实 `INetFwRule` 内存对象验证 UDP、TCP 属性设置及读回。Release `chast-host`、`chast-service` 重建通过；`firewall_rule`、`firewall_transaction`、`service_install`、`service_uninstall` CTest 4/4 PASS。测试只创建并配置内存 COM 对象，没有访问全局 Rules 集合或调用 `Rules.Add`，因此真实规则创建与失败回滚仍待专用可回滚测试机验收。

2026-11-20 COM 属性顺序修复后完整 P1 复跑：18 项中 17 项 PASS，`machine_state_security_untrusted_owner` 因无法启用 `SeRestorePrivilege`（错误 1300）单独 Skipped；总耗时 190.05 秒，`host_parity` 124.71 秒通过。测试后只读检查确认没有 `chast-host` 服务或产品进程、没有 `%ProgramData%\chast\host.ini`、没有四条产品防火墙规则。该结果不替代真实 SCM 生命周期、真实 COM `Rules.Add`/回滚或 owner 集成验收；未执行正式安装/卸载、双机测试或防火墙写入。

### P2：第二台 Windows 与跨网

2026-11-20 SCM 停止失败恢复补强：安装与卸载事务现都在发出停止请求前登记原运行/暂停状态恢复动作；停止失败或超时后先查询当前状态，仍在目标状态则无操作，STOP_PENDING 则等待停止完成后再恢复原状态，PAUSED/RUNNING 之间按需继续或暂停。扩展 fake SCM 回归覆盖停止超时、恢复失败错误传播、已运行不重复启动、STOP_PENDING 后重启，以及 START_PENDING、CONTINUE_PENDING、PAUSE_PENDING 的等待/恢复路径。Release `chast-service`、`chast-host` 构建通过；`service_stop`、`service_install`、`service_uninstall` 定向 CTest 在新增过渡态断言后 3/3 PASS，完整 P1 复跑 18 项中 17 项 PASS、owner 专项因错误 1300 Skipped，188.52 秒。测试未创建/控制真实服务；只读后检查仍无 chast 服务/产品进程、机器配置文件或产品防火墙规则，正式 SCM 生命周期仍待专用可回滚测试机验收。

2026-11-20 P1 最新代码复验：增量 Release 重建 `chast-service`、`chast-host` 及全部 18 个 P1 测试目标成功（保留既有 LNK4199/LNK4098 链接警告）；完整定向 CTest 18 项中 17 项 PASS，`machine_state_security_untrusted_owner` 因当前令牌无法启用 `SeRestorePrivilege`（错误 1300）单独 Skipped，总耗时 190.60 秒，`host_parity` 124.85 秒通过。`tools/test-service-scm.ps1` Parser 和默认只读预检 PASS；复验后只读确认无 `chast-host` 服务/进程、无 `%ProgramData%\chast\host.ini`、无四条产品防火墙规则。`git -c core.whitespace=cr-at-eol diff --check` PASS。未执行服务安装/卸载、验收脚本 `-Execute`、防火墙写入或双机测试；真实 SCM 生命周期、真实 COM `Rules.Add`/回滚及 owner NTFS 集成场景仍待可回滚测试机验收。

2026-11-20 完整代码门禁复验：`server` 的 `go test ./...` 和 `go vet ./...` PASS（Go 测试包命中有效缓存）；Flutter `analyze` PASS、`test` 815/815 PASS；管理台 Node 测试 48/48 PASS、`node --check` PASS；Flutter Windows Release 构建 PASS（40.6 秒，保留既有 LNK4199/LNK4098 警告）。`tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -InputInjection` PASS（12.068 秒）：A-up 在 release 区间内且位于物理清理边界前，边界后无重复远端事件；输入使用 dry-run，测试 profile 隔离。证据：`build/native-runtime-20261120-020330-0137235e/run-1594865917/`。三个原生验收 PowerShell 脚本 Parser 均 PASS；随后 `tools/test-service-scm.ps1` 默认只读预检 PASS，确认无产品服务/进程、机器设置或防火墙规则。以上未执行 SCM 安装/卸载、防火墙写入或双机验证。

2026-11-20 SCM 安装幂等策略审查修正：复核发现此前状态记录所述策略与生产实现不一致，既有服务每次安装仍会无条件停止，且暂停/过渡态未被拒绝。现改为仅接受 STOPPED/RUNNING；路径/启动类型或机器设置无变化的运行中服务不停止、不重启，检测到上述配置变化才先登记原运行状态恢复动作并停服；启动、停止、暂停及其他过渡态在机器目录/ACL操作前拒绝。后续回查又发现服务路径匹配器误将当前路径与自身比较，已修正为比较当前与目标配置，并在 fake SCM 测试中覆盖相同配置、路径、命令行及启动类型变化。另新增幂等运行不间断、机器设置变化时停服恢复、五种暂停/过渡态拒绝断言。Release `chast-service`、`chast-host`、`chast-service-install-test` 构建通过；最终完整 P1 CTest 18 项中 17 PASS、`machine_state_security_untrusted_owner` 因错误 1300 单独 Skipped，188.22 秒，`host_parity` 123.63 秒通过。Flutter Windows Release 在全部服务和进程封装改动后再次构建通过（40.3 秒，保留 UNICODE/getenv/LNK4098 警告）。SCM 默认只读预检确认无服务/产品进程、机器设置或产品防火墙规则；未执行真实服务安装/卸载或防火墙写入。真实 SCM 验收仍需专用可回滚测试机。

2026-11-20 `platform_process` 偶发清理断言加固：完整 P1 曾在“超时子进程已退出”即时断言失败一次；测试原先在 `run_hidden_command()` 关闭进程句柄后按 PID 重开，存在进程标识竞态。现允许调用者可选接收其创建时的进程句柄，测试直接检查该原始句柄并负责关闭；修正后专项连续 20 次 PASS，最终完整 P1 中该项也 PASS。Release `chast-service`、`chast-host`、`chast-platform-process-test` 与 `chast-service-install-test` 构建通过；未做系统服务或防火墙写入。

2026-11-20 机器状态 owner 迁移权限作用域修复：代码审查发现 `SeRestorePrivilege` 成功启用后未恢复进程令牌原状态。现以作用域对象保存并恢复原 privilege attributes；owner 更新后恢复失败会使 ACL 加固失败并返回错误，不会静默继续。测试增加隔离临时目录迁移前后令牌属性比较；当前令牌无法启用该权限（`ERROR_NOT_ALL_ASSIGNED`，1300），因此 owner/privilege 集成专项 Skipped，不能据此宣称恢复行为已在本机集成通过。`ctest --test-dir build\root -C Release -R machine_state_security --output-on-failure` 结果为基础 ACL 测试 PASS、owner 专项 Skipped。SCM 脚本默认只读预检再次 PASS，确认无产品服务/进程、`%ProgramData%\chast\host.ini` 或四条产品防火墙规则。此前启动的 Flutter Windows Release 增量构建完成，`chast_viewer.exe` 成功生成（13.0 秒；保留 LNK4199/LNK4098 链接警告）。本次没有改动真实 ProgramData/ACL、SCM 或防火墙，也未做双机测试。

2026-11-20 W2 C01 查看端防休眠实现：Windows 查看端新增 `PowerCreateRequest`/`PowerRequestSystemRequired` 会话请求，并用进程级引用计数让多个会话共享、最后一个会话释放后清理；Dart 会话启动时可选获取，请求失败不阻断连接，关闭及 native stop/bridge shutdown 都有释放兜底。新增 fake callback 租约测试，CTest `reference_counted_lease` 1/1 PASS；`flutter analyze --no-pub` PASS、`session_start_polling_test.dart` 2/2、全量 Flutter 测试 819/819 PASS；Windows Flutter Release 首次编译发现重复 `NOMINMAX` 严格警告并已修复，随后构建 PASS（保留既有 LNK4098/LNK4199 警告）。没有启动远端会话或改动系统电源设置，因此真实防休眠效果、多会话实机释放及双机验证仍待后续。C01 的默认仅查看持久化偏好与主机 LAN 发现设置已纳入实现；发布验收仍依赖 W1 外部验收和后续实机证据。

2026-11-20 W2 C01/C02 单机复验：C01 `reference_counted_lease`、`viewer_awake_request` Release CTest 均重建并通过（2/2）；Flutter `analyze --no-pub` 无问题、全量测试 825/825 通过，Windows Flutter Release 构建成功。当前工作区 C02 已实现按设备保存的 5–1000% 视图缩放及 25–400% 独立光标倍率，并接入主会话、桌面多显示器布局和独立显示器窗口。定向测试 30/30 通过，覆盖缩放下源像素输入映射、光标热点/倍率、普通滚轮远端转发与 Shift+滚轮本地平移；另在固定逻辑窗口尺寸下模拟 DPR 从 1 到 2，验证相对输入倍率同步更新，并覆盖旋转后尺寸与坐标映射、多显示器网格和桌面拼接缩放。真实混合 DPI、旋转、全屏和多屏显示效果尚未验收，故 C02 仍需场景验收。全程未启动远端会话，未更改持久电源策略、SCM、ProgramData 或防火墙；双机验证继续后置。

2026-11-20 W2 C03 首个实现切片：独立远端显示器窗口首次打开时，根据服务端公布的远端显示器矩形及虚拟桌面范围，将布局等比例映射到本机虚拟桌面；单显示器远端窗口映射到本机主显示器。逐设备/远端显示器保存的位置仍优先，拔掉保存位置所在本机屏幕后会忽略不可见旧位置并采用自动映射。几何 Release CTest 1/1、Flutter analyze、全量测试 825/825、Windows Release 构建通过。多连接标签页、会话音频/录制状态隔离及运行中拓扑变化后的本机窗口重新布局未实现；本轮未连接远端或更改系统显示配置。

2026-11-20 W2 C03 本机热插拔恢复切片：独立显示窗口收到 `WM_DISPLAYCHANGE` 后会在其当前可用区域不足 320×240 时重新按远端布局映射，并在显示拓扑尚未稳定时每 250 ms 重试、最多约 10 秒；已存位置也要求至少有 320×240 可见交集。几何/当前显示器只读 Release CTest 1/1、Windows Release 构建通过。未实际拔插显示器，物理热插拔及混合 DPI 验收仍待完成。代码审计确认独立连接窗口各有 `SharedViewerSession`/`ViewerEngine`，会话音频/麦克风/语音/录制状态由各引擎分别持有；远端显示子窗口共享连接但桥接层不开放这些会话级操作。多会话运行时隔离回归与多连接标签页尚未实现。

2026-11-20 W2 C03 单机双会话隔离回归：新增 `chast-viewer-probe --viewer-session-isolation`，由两个独立 `ViewerEngine` 通过本机 QUIC loopback 连接同一主机。Release 目标构建通过（保留既有 LNK4098）；CTest `viewer_session_isolation` 1/1 PASS，手动探针也通过：分别设置不同 AEC/降噪选项并启动本地录制，停止 A 后确认 B 仍在收流且录制，最后两份 VP9 MP4 均成功重开校验（A 12 samples/3 keyframes，B 40 samples/5 keyframes）。既有 `--host-recording` 模式也通过。证据目录：`test-temp/chast-viewer-session-isolation-probe-14872-51419625/`。该回归只证明本机 native 双引擎状态与录制隔离；没有音频负载，未验证实际 DSP 效果、Flutter `SharedViewerSession`/界面生命周期、物理热插拔或双机。未发送键鼠/麦克风输入，也未更改系统配置。

2026-11-20 W2 C03 窗口位置持久化回归：扩展 `display_layout`，在唯一系统临时目录验证窗口边界按 peer/display 索引保存与读取、更新只替换目标记录、不影响其他 peer/display，并确认未保存索引不继承位置。Release `chast-display-layout-test` 构建通过，CTest `display_layout` 连续 10 次通过；测试只枚举当前显示器并在结束时清理自己创建的临时目录，没有访问产品 `%AppData%` 配置或更改显示拓扑。该单测不模拟真实 `WM_DISPLAYCHANGE`、拔屏或窗口消息路由。

2026-11-20 W2 C03 Flutter 多窗口连接隔离验收：新增 `tools/test-native-runtime.ps1 -Flutter -MultiWindow` opt-in 模式，复用隔离 APPDATA 与本机 loopback 主机，在同一个 Flutter 进程内从首页打开两个独立会话窗口。实际报告两条连接后按目标 connection ID 精确断开其中一条，签名缺席回执被确认，另一连接仍为 active，Flutter 进程继续运行。`go test ./internal/api -run '^TestWindowsNativeRuntimeAcceptance$'` PASS（26.1 秒），Flutter Windows Release build PASS；证据：`build/native-runtime-20261120-063259-c734d6d1/run-1081652127/`。主机配置 `input=false`，测试未发送键鼠/麦克风输入、未安装服务或改机器设置。此项证明同进程多 Flutter 窗口的连接隔离，不等同于逐窗口录制/音频控制 UI、实际 DSP、物理热插拔或双机验收。

2026-11-20 查看端录制文件名冲突修复：默认路径按秒生成文件名，旧打开方式可截断同名文件；并发 ViewerEngine 或旧录制文件可能被覆盖。`Mp4Recorder` 现仅在首个关键帧到达时，以 Windows CRT `O_CREAT|O_EXCL` 原子占用文件名；冲突时试下一个编号，续段同样独立占位；首帧前不创建文件，初始化失败会清理本次占位。新增两录制器并发起录、并发分段、旧文件哨兵保留及无关键帧不落空文件的回归。Release `chast-mp4-recorder-test`、`chast-viewer-probe`、`chast-quic` 构建通过；隔离工作目录中 MP4 专项测试 PASS，`chast-viewer-probe --viewer-session-isolation` loopback 复验 PASS（A 11 samples/2 keyframes，B 36 samples/4 keyframes）。日志及生成文件位于 `build/test-temp/mp4-exclusive-run-b95d76d7b21c4b9a9d0af6c7b9a394f1/`；构建保留既有 LNK4098/LNK4199 告警。没有启用麦克风或执行双机/服务/防火墙操作，单机证据不替代双机验收。

2026-11-20 W2 C03 显示子窗口媒体控制：独立显示器子窗口新增会话级系统声音静音、麦克风上行、主机麦克风语音通话和本地录制按钮；声音按钮长按可选输出设备，麦克风按钮长按可选输入设备并切换 AEC/降噪。按钮共用原生 `ViewerEngine` 状态轮询，因此同一会话的多个显示子窗口同步图标/权限状态，独立会话仍由各自引擎隔离；操作期间显示忙碌状态，平台错误、授权拒绝、录制错误有提示。主会话观察到其他窗口停止录制后会抑制本会话的自动录制重启。双显示窗口 Flutter widget 回归覆盖状态同步、音频设备/麦克风设备和处理选项选择、麦克风拒绝及撤销权限后的禁用状态；另有回归证明子窗口停止录制不会被主窗口自动录制策略重新启动。`flutter analyze --no-pub` 无问题、全量测试 827/827 通过，Windows Flutter Release 构建 47.7 秒通过（保留现有 native 链接警告）。

2026-11-20 C03 合成音频 loopback：检查并运行现有 `build\Release\chast-quic.exe verify --port 45671 --seconds 5 --audio --microphone --quiet`。整体验证通过，视频 147/147 access units 解码；系统声音由合成 Opus 48 kHz 立体声源产生，发送/接收包、FIN、解码时长和预期能量均通过，无发送错误/丢包、CRC、协议或解码错误；麦克风使用合成 48 kHz 单声道 Opus 源，上行 CMIC 包全部到达并由主机解码，440 Hz 和能量检查通过；无权限主机拒绝 CMIC 且视频继续。没有使用 `--play`，未通过 WASAPI 播放器输出声音，也没有采集真实麦克风、操作实际键鼠或改动系统音频设置。这证明协议负载与解码链路，不代表实际音频设备/DSP 验收。

2026-11-20 C03 WASAPI 播放测试（使用当前源码重建的 `chast-quic`）：无弱网命令 `build\Release\chast-quic.exe verify --audio --play --seconds 25` 通过；Opus 48 kHz 立体声 2499 包无缺口/解码错误，解码 24.99 秒；播放 1176960 frames、2 次 underrun、400 ms starved、42 dropped、audio-latency 30.0 ms、A/V skew +22.8 ms。既有稳定链路 ±40 ms skew 目标在该单次样本中达到。默认弱网命令 `build\Release\chast-quic.exe verify --netem --audio --play --seconds 25` 也通过，音频 2500 包发送、2494 包接收（0.24% 丢包），解码 24.94 秒，无 CRC/解码/协议错误；WASAPI 播放 804960 frames、2 次 underrun、8170 ms starved、812 dropped、audio-latency 26.1 ms、A/V skew +18.0 ms。弱网单次比干净基线多 7770 ms 空转和 770 个丢弃帧。验证断言不限制这些播放连续性指标，不能把弱网整体验收通过等同音质达标；两档目前都只有一轮结果。

2026-11-20 C03 真实麦克风 loopback：构建 `chast-viewer-probe` 后运行 `build\Release\chast-viewer-probe.exe --real-microphone`。ViewerEngine 打开默认 `麦克风 (USB Audio Device)`，实际 WASAPI 采集并经仅绑定 loopback 的 QUIC 发送；174 个上行包中主机解码 173 个，收到 83160 个采样，RMS 0.4、峰值 27；AEC/降噪处理计数分别为 39840/41280 帧。撤销会话麦克风权限后采集停止，视频又收到 15 帧；仅保留统计值，没有录制文件或扬声器回放。该结果证明实际设备采集、编码、传输、解码和撤权路径；输入电平很低且 host 未发送下行音频参考，不证明可懂语音或 AEC/降噪效果。

#### 2026-11-20 C03 多轮对照与本次归档状态

当前源码在查看端系统声音播放器启用已有 `adaptive_jitter`，并收紧真实麦克风探针的身份边界：仅查询并复用当前用户已有 Chast 证书；找不到身份时退出，不创建证书存储、证书或密钥。此前探针通过且运行前后当前用户证书集合一致。麦克风数据仅统计，不落录音文件；这不补足可懂语音与实际 DSP 效果证据。

以下四组对照均使用二进制 SHA-256 `DF90FB011099D51D8DD5427B9808F42FEDA2BD277E8462929F4E27C4C6C3AC52`，构建时开发基线为 `172516e` 且存在未提交改动。每轮会话 25 秒；默认弱网为 80 ms 基础延迟、0–20 ms 抖动、3% 丢包、1000 kbps，4–12 秒劣化窗口。固定/自适应为同二进制运行配置对照；各组按组执行，不是逐轮交替实验。`starved` 是播放空转毫秒，`dropped` 是过期或超队列上限而丢弃的音频块，不能写成 PCM 帧数。

| 场景（各 5 轮） | 空转中位数（范围，ms） | 丢弃中位数（范围，块） | underrun 中位数（范围） | A/V skew 范围（ms） | 整体验证 |
| --- | --- | --- | --- | --- | --- |
| 固定缓冲，默认 3000 kbps 视频 | 8350（8070–9200） | 830（787–908） | 4（1–15） | -5.0–+23.9 | 5/5；未检查音频连续性 |
| 自适应缓冲，默认 3000 kbps 视频 | 3840（2980–4510） | 369（282–433） | 20（10–21） | +18.6–+48.3 | 4/5；第 1 轮最大视频帧龄 3048 ms，超过 3000 ms 门槛 |
| 自适应缓冲，1200 kbps 起始视频 | 1670（1550–2840） | 152（140–266） | 7（3–30） | +23.0–+41.1 | 5/5；码率/帧率有取舍 |
| 自适应缓冲，可靠音频流，默认视频 | 7200（6930–8060） | 709（682–795） | 7（5–9） | +21.0–+43.4 | 5/5；未优于数据报播放 |

逐轮原始指标摘录，轮次与各目录日志文件后缀一致：

| 场景 | 轮次 | starved（ms） | dropped（块） | underruns | skew（ms） | 最大视频帧龄（ms） |
| --- | --- | --- | --- | --- | --- | --- |
| 固定缓冲 | 1 | 8850 | 882 | 4 | +15.5 | 2378 |
| 固定缓冲 | 2 | 8350 | 830 | 5 | +21.7 | 2038 |
| 固定缓冲 | 3 | 8120 | 787 | 4 | -5.0 | 2278 |
| 固定缓冲 | 4 | 9200 | 908 | 15 | +14.4 | 2144 |
| 固定缓冲 | 5 | 8070 | 802 | 1 | +23.9 | 1928 |
| 自适应默认码率 | 1（FAIL） | 4510 | 433 | 20 | +18.6 | 3048 |
| 自适应默认码率 | 2 | 2980 | 282 | 10 | +34.5 | 758 |
| 自适应默认码率 | 3 | 4360 | 417 | 15 | +31.4 | 1363 |
| 自适应默认码率 | 4 | 3840 | 369 | 21 | +48.3 | 967 |
| 自适应默认码率 | 5 | 3760 | 360 | 21 | +38.1 | 896 |
| 自适应 1200 kbps | 1 | 1550 | 140 | 7 | +38.9 | 628 |
| 自适应 1200 kbps | 2 | 1560 | 140 | 5 | +27.9 | 639 |
| 自适应 1200 kbps | 3 | 2560 | 239 | 8 | +38.9 | 644 |
| 自适应 1200 kbps | 4 | 1670 | 152 | 3 | +41.1 | 589 |
| 自适应 1200 kbps | 5 | 2840 | 266 | 30 | +23.0 | 1166 |
| 可靠音频流 | 1 | 6960 | 685 | 9 | +21.0 | 1544 |
| 可靠音频流 | 2 | 6930 | 682 | 8 | +43.4 | 2110 |
| 可靠音频流 | 3 | 7450 | 734 | 7 | +38.3 | 2263 |
| 可靠音频流 | 4 | 8060 | 795 | 6 | +39.7 | 2371 |
| 可靠音频流 | 5 | 7200 | 709 | 5 | +40.5 | 2291 |

原始日志分别在 `build/test-temp/c03-wasapi-default-fixed-current-20261120/`、`c03-wasapi-default-adaptive-current-20261120/`、`c03-wasapi-1200kbps-20261120/`、`c03-wasapi-audio-stream-20261120/`，各目录有 `metadata.txt`。日志及构建产物留在本机，不纳入源码提交；本表保留可在 GitHub 审阅的指标。

较早干净链路自适应 5 轮空转中位数 430 ms（380–520），丢弃中位数 41 块（35–50），underrun 均为 2，skew +10.9–+24.2 ms；证据 `build/test-temp/c03-wasapi-adaptive-clean-20261120/`。其二进制 SHA-256 为 `5B6E4BE5CBA380E7D109E35B1F7A27247ED42B103BB4D3C58E54059744302A62`，不同于上述四组，不能作为同版本交替干净对照。

保留已学缓冲余量的临时实验空转中位数 4910 ms，未改善且出现视频帧龄失败，已从源码撤回；`c03-wasapi-reanchor-adaptive-20261120/` 的二进制不代表最终源码。本次归档重新构建最终原生目标，避免把实验产物误当最终版本。

#### 2026-11-21 C03 到达迟到与 WASAPI 恢复诊断

弱网探针新增 WASAPI 恢复原因、HRESULT、渲染调用失败数、消费停滞重开数、默认设备变化数及重开失败数。随后运行 10 轮、每轮 25 秒的交替对照，参数与查看器一致：静音 WASAPI 输出、10 ms 请求缓冲、`--adaptive-jitter --audio-arrival-phase-correction`，只将到达迟到容忍从默认 20 ms 改为 100 ms。弱网配置为 80 ms 基础延迟、0–20 ms 抖动、3% 丢包、1000 kbps、4–12 秒劣化窗口。原始日志位于 `build/test-temp/c03-gui-playout-late-tolerance-20261121-run01-ms20.log` 至 `run10`；运行中的日志名包含对应 `ms20` 或 `ms100`。这些整体验证同时检查 3000 ms 视频帧龄门槛，但门禁没有音频空转、丢弃或 A/V skew 的连续性阈值。

| 迟到容忍 | 空转中位数（范围，ms） | 丢弃中位数（范围，块） | 接受迟到块中位数 | A/V skew 中位数（范围，ms） | 最大视频帧龄中位数（范围，ms） | 弱网门禁 |
| --- | --- | --- | --- | --- | --- | --- |
| 20 ms（当前默认） | 1285（1006–2076） | 69（46–151） | 1488 | +57.5（+50.7–+69.2） | 1481（1237–4045） | 4/5 |
| 100 ms（实验） | 1150（1095–1519） | 50（34–69） | 1574 | +136.2（+132.5–+147.7） | 2137（1247–4084） | 4/5 |

实验设置中 100 ms 的空转中位数低 135 ms、丢块中位数低 19 块，但 skew 中位数增加 78.7 ms；两种设置的稳定链路 ±40 ms skew 目标均未达到。两组各有一轮最大视频帧龄超过 3000 ms（20 ms 组 4045 ms，100 ms 组 4084 ms），失败不只出现在扩大容忍的一组，不能据此归因于容忍值。**保留产品默认 20 ms，不将 100 ms 作为默认优化。**

10 轮中，接收端每轮报告 86–173 个乱序包；20 ms 重排等待仍有序号缺口并调用 Opus 隐藏补偿（每轮隐藏 20–28 个块），无 CRC 或解码错误，隐藏块数始终不超过缺失序号数。接收队列没有溢出；播放队列 10 轮仅有 3 轮发生溢出，每轮最多 2 块。因此本组证据不支持把 PCM 队列从 500 ms 扩至 RustDesk 的 3000 ms。10 轮均报告 `recoveries=0`、`render-failures=0`、`stalled=0`、`device-changes=0`，之前无相位修正样本里的两次端点重开没有复现，根因仍未确定。

Release `chast-quic`、`chast-audio-playout-test` 构建通过；`audio`、`audio_dsp`、`audio_playout`、`protocol`、`viewer_audio_video_isolation` 五项 CTest 通过，独立 `quic_weak_network` CTest 通过。后者不运行音频播放。当前 5 FPS 单机音视频隔离回归通过，但这不替代弱网音频连续性门槛。

为区分视频门禁波动是否只来自本机播放输出，再用相同 25 秒弱网跑了两组五轮：视频-only 最大帧龄中位数 1421 ms（1135–1576），5/5 通过；启用系统音频发送和查看端解码、但不播放的对照中位数 2244 ms（1777–3676），4/5 通过。启用查看端播放的两种迟到设置各 4/5 通过。音频开启的样本表现出较高帧龄及失败相关性，但顺序单机测试不足以确认因果。已核实主机存在音频码率预留：`bitrate_reserve` 设为通告音频码率的 110%，`DesktopStreamPool::quality_bitrate` 会从视频目标中扣除该预留；当前不能把现象简单归因于 ABR 完全忽略音频。下一步应检查预留与实际编码/发送速率、IDR 传输及共享链路队列在音频开启时的关系，并继续维持现有视频门禁。

**当前结论：C03 弱网音频连续性仍未达标，既有视频帧龄门禁也有失败样本；不将整体 PASS 写成音质通过。** 暂不降低产品默认视频码率或改用可靠音频流。保留 20 ms 迟到容忍，不采用 100 ms 实验值。下一步定位视频竞争、排队与播放迟到，建立可失败的连续性门槛；修复后再做同最终二进制的干净/弱网各至少 5 轮交替验收，同时检查画质、帧率、skew 与最大视频帧龄。

#### 2026-11-21 音频包到达间隔与带宽队列隔离

在 QUIC DATAGRAM 有效音频包回调处增加包间最大到达间隔和超过 100 ms 的间隔计数，并在逐秒轨迹、最终接收汇总和弱网汇总输出。间隔从本机单调时钟测量，不依赖双机时钟同步；它统计网络回调间隔而非音频内容本身。Release 探针二进制 SHA-256：`B5FEA93FBB484224F3ADEF6A9DF2E85AA2B9FCBFBBFA3E4DB868B6316E8BFD2A`。六轮逐轮交替弱网对照采用同一二进制、10 ms WASAPI 请求缓冲、自适应 jitter 与相位修正、20 ms 迟到容忍，时长 25 秒；只改变初始视频码率 3000/1200 kbps。原始日志在 `build/test-temp/c03-audio-arrival-gap-20261121-*.log`。

| 初始视频码率 | 播放空转中位数（范围，ms） | 播放丢弃中位数（范围，块） | 包间最大间隔中位数（范围，ms） | 每轮 >100 ms 间隔（范围） | relay 排队峰值中位数（范围，ms） | relay 队列丢弃中位数（范围） | 最大视频帧龄中位数（范围，ms） |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 3000 kbps，3 轮 | 1829（1501–1829） | 117（83–160） | 162.4（153.6–206.6） | 6–12 | 589（581–591） | 45（21–58） | 1684（1134–2025） |
| 1200 kbps，3 轮 | 1054（852–1121） | 71（69–89） | 126.3（105.2–140.7） | 1–8 | 238（187–280） | 0（0） | 946（821–1610） |

这组有限样本中，较低初始视频码率伴随较短音频空转和 relay 排队，但不能据此下调产品默认码率。两档均有音频包迟到或缺失，接收队列峰值仅 6–7 包、最大驻留 42.9–60.5 ms；探针音频轮询最大间隔为 12.4–20.5 ms，单次解码并入队耗时不超过 0.5 ms。证据不支持把问题归结为接收队列持续堆积或 Opus 解码长时间阻塞。

随后用同一组弱网参数进行隔离对照，均保持初始视频码率 3000 kbps。固定 1000 kbps 且关闭随机丢包的最近五轮均通过修正后的原有视频帧龄/协议门禁；移除带宽上限但保留 3% 随机丢包的四轮中有一轮因最大视频帧龄 3399 ms 超过 3000 ms 而失败。该原有门禁不检查音频连续性：

| 80 ms 延迟、0–20 ms 抖动 | 带宽上限 | 随机丢包 | 轮数 | 播放空转中位数（范围，ms） | relay 排队峰值中位数（范围，ms） | relay 队列丢弃中位数（范围） | 最大视频帧龄中位数（范围，ms） | 原有弱网门禁 |
| --- | --- | --- | ---: | --- | --- | --- | --- | --- |
| 开 | 无 | 3% | 4 | 352（326–462） | 100（100） | 0（0） | 2659（1823–3399） | 3/4 |
| 开 | 1000 kbps | 0% | 5 | 2664（1767–3973） | 593（587–597） | 74（53–115） | 1273（1084–2036） | 5/5 |

带宽整形源码对每个方向分别使用共享的 FIFO `next_free` 时序；中继看到的是加密 QUIC UDP 数据报，不能从包内容区分音频并提供优先级。受限带宽、零配置随机丢包组仍出现显著播放空转和队列丢弃；保留延迟/抖动/3% 配置丢包但移除带宽上限组则空转较少。注意后者四轮实收 QUIC 包丢失为 0.28%–1.04%，并非绝对无损；同轮 relay 统计能区分配置丢包与实际数据报丢失。这使“受限共享带宽下的队列排队/视频竞争”成为当前更强的故障方向，而非单纯随机丢包。两组没有严格保持相同的实际发送负载，且结果来自单机 25 秒模拟，仍不足以证明真实链路上的充分因果或直接选择产品调度策略。

为区分主机积压与提交后的链路排队，在主机 drain 音频时增加只读的 QUIC 提交间隔和 capture-to-submit 年龄测量，并传至弱网汇总。限速组最新四轮的最大提交间隔为 30–35 ms、最大采集到提交年龄为 36–44 ms；无带宽限制组两个新测轮分别为 32–37 ms 和 39–42 ms。相较之下，1000 kbps 限速组的 QUIC 回调最大包间隔为 202.5–450.9 ms，设备播放最大断档为 457–1976 ms。综合这些单机证据，采集线程和其队列不是秒级中断的主要位置；音频提交 QUIC 后到达得过晚或成簇，播放端再表现为空转、迟到丢弃和溢出。四轮主机时序测量仍不代表长时或真实设备的最坏值。

无随机丢包限速组首轮日志 `c03-audio-isolation-20261121-rate1000-loss0-01.log` 曾因弱网探针只断言随机丢包计数大于零而失败，尽管限速器已经记录到视频队列丢弃。将断言修正为随机丢包或队列丢弃任一成立后，最近五轮均通过；首轮日志保留且不作为通过轮次。四轮无带宽上限组有一轮因既有视频帧龄门禁失败，其音频指标仍保留为实测值。另两轮非弱网 loopback 日志 `c03-audio-arrival-gap-clean-20261121-*.log` 观测到单次 289–321 ms 的包间最大间隔，但每轮超过 100 ms 仅 1 次、播放空转 353–397 ms；最大包间隔不能单独充当断续判据。

代码阅读补充：WASAPI 采集在独立线程编码，采集队列上限 32 包；共享 loopback 对每个订阅另有 25 包上限和 500 ms 年龄淘汰。主机在视频抓取等待返回后批量 drain 音频，再编码/发送该轮视频；音频调用非阻塞 `DatagramSend`，视频在另一条 QUIC 流调用非阻塞 `StreamSend`。视频应用交付窗口以 8 MiB、60 帧和 500 ms 限制已确认帧积压，且允许空窗口通过一个大 IDR。探针已加入 capture-to-submit 年龄、音频提交间隔、包到达间隔与播放/队列阶段统计；目前仍缺少可归因的逐帧/IDR 大小、发送时刻与音频断续时间相关分析。

进一步查阅构建所用 MsQuic **2.4.8** 源码后发现，当前音频 DATAGRAM 发送使用 `QUIC_SEND_FLAG_NONE`，而 API 提供了 `QUIC_SEND_FLAG_DGRAM_PRIORITY`：它把音频排在 MsQuic 内部的其他普通 DATAGRAM 前面。MsQuic 的连接发送实现先处理 connection-wide DATAGRAM 工作，然后才写 STREAM 帧；但 DATAGRAM priority 不会抢占已提交到路径/网络中的数据，也不是跨 UDP 包的带宽保证。音频发送现已设置该优先标志。

为比较标志本身的效果，使用相同限速、初始视频码率 3000 kbps、10 ms WASAPI 缓冲、自适应 jitter、相位修正、20 ms 迟到容忍和 25 秒时长，各跑五轮；无标志组按“完成后”轮次取同一组近期数据，测试顺序不是随机交替，故仍受单机负载和样本波动影响：

| MsQuic 音频 DATAGRAM 标志 | 播放空转中位数（范围，ms） | 播放丢弃中位数（范围，块） | 接收包间最大间隔中位数（范围，ms） | 播放设备最大断档中位数（范围，ms） | relay 排队中位数（范围，ms） | 弱网视频门禁 |
| --- | --- | --- | --- | --- | --- | --- |
| 默认 `NONE` | 2664（1767–3973） | 164（98–329） | 377.1（202.5–450.9） | 1631（457–1976） | 593（587–597） | 5/5 |
| `DGRAM_PRIORITY` | 3008（1256–3386） | 189（56–235） | 278.3（219.0–473.4） | 638（429–1952） | 591（588–596） | 5/5 |

`DGRAM_PRIORITY` 组的单项指标方向不一致：包间隔与设备断档中位数较低，但累计空转和播放丢弃没有改善；relay 拥塞排队没有变化。因此保留该标志作为对 MsQuic 普通数据报队列的正确标注，**不宣称它修复了弱网音频连续性**。在它之外，短关键帧前置视频发送、独立音频带宽预算、渐进视频突发整形等方案还没有实现或验收。两组都没有音频空转、丢弃或 A/V skew 连续性门禁。

此次增加了只读音频提交/到达诊断，修正“有限速但没有随机丢包”的弱网测试断言，并给 QUIC 音频 DATAGRAM 设置 `DGRAM_PRIORITY`；未改编解码、播放策略、PCM 容量、迟到容忍或默认码率。五轮测试尚未证明该标志能改善累计空转，音频连续性仍无失败门禁。下一步应增加实际 DATAGRAM 提交/发送状态时间点及 IDR 大小/发送时刻关联分析，随后对照验证限制关键帧/视频突发同时维持音频的候选策略；并检查帧龄、画质/帧率、A/V skew。不能把移除码率上限的测试泛化成不需争取带宽，也不能据此直接降低产品默认码率。最终 Release 构建后，`protocol`、`audio`、`audio_dsp`、`audio_playout`、`viewer_audio_video_isolation`、`quic_weak_network` 六个 CTest 必须复通过。

#### 2026-11-21 视频在途窗口受控对照

为限制 IDR/视频突发对共享链路的占用，短暂实现了仅用于 `chast-quic` 探针的 `--video-send-window-kb 64`：按 MsQuic `StreamSendComplete` 释放视频在途字节；空窗口仍允许一个完整 access unit，因此实测峰值可超过 64 KiB。没有启用产品默认值。移除此试验实现前使用同一 Release 二进制 SHA-256 `62FB82EF6F9C53989CCDBB9214A190BE4A14C71EF00D8A4843245DD113DB229F`，以基线、64 KiB、64 KiB、基线顺序跑四轮 ABBA；每轮 25 秒，均开 WASAPI 静音播放、自适应 jitter/相位修正、10 ms render buffer、20 ms late tolerance。默认弱网配置为 80 ms 延迟、0–20 ms 抖动、3% 丢包、1000 kbps 限速，4–12 秒劣化。日志为 `build/test-temp/c03-video-send-window-baseline-02-20261121.log`、`c03-video-send-window-64k-02-20261121.log`、`c03-video-send-window-64k-03-20261121.log`、`c03-video-send-window-baseline-03-20261121.log`。

| 指标（两轮中位数） | 基线 | 64 KiB 在途窗口 |
| --- | ---: | ---: |
| WASAPI 播放空转 | 1443 ms | 1118 ms |
| >100 ms 音频包间隔数 | 9 | 11.5 |
| 最大音频回调间隔 | 229 ms | 333 ms |
| relay 队列峰值 | 586 ms | 460 ms |
| 最大视频帧龄 | 1749.5 ms | 2464.5 ms |

64 KiB 组设备最大断档中位数为 375 ms，基线为 681.5 ms，但播放空转和视频帧龄在单轮间波动明显；更重要的是，64 KiB 组一轮最大帧龄达到 3758 ms，超过已有 3000 ms 门禁并导致探针失败。限额允许一个超过限额的 access unit 独占空窗口，实测在途峰值为 90/103 KiB。音频最大回调间隔反而从 229 ms 升至 333 ms。该小样本不证明窗口能改善音频，且有明确画面时效回归；因此不采纳也不保留这个实验开关，不降低产品码率或调整默认调度。原有弱网 CTest 不包含音频连续性门槛，单轮播放统计不能替代连续性验收。

#### 2026-11-21 接收端重排等待 ABBA 对照与连续性门槛

使用同一 Release 探针二进制 SHA-256 `8190816A75722017BC81A260341F7C78A8B74BAF3F4DD19C72CB4693D0ACCEE5`，按 20/50/50/20 ms 顺序运行四轮 ABBA。各轮均为 25 秒、WASAPI 静音输出、自适应 jitter/到达相位修正、10 ms 端点请求、20 ms 迟到容忍；弱网为 80 ms 延迟、0–20 ms 抖动、3% 随机丢包、1000 kbps、4–12 秒劣化。四轮原有弱网/视频门禁均通过。原始日志：`c03-audio-reorder-wait-20ms-01-20261121.log`、`c03-audio-reorder-wait-50ms-01-20261121.log`、`c03-audio-reorder-wait-50ms-02-20261121.log`、`c03-audio-reorder-wait-20ms-02-20261121.log`。

| 指标（每档两轮中位数） | 20 ms | 50 ms |
| --- | ---: | ---: |
| WASAPI 累计空转 | 1308 ms | 1629.5 ms |
| 播放丢块 | 68.5 | 83.5 |
| >100 ms 音频包间隔数 | 9 | 7.5 |
| 最大音频包到达间隔 | 244.9 ms | 269.2 ms |
| 播放设备最大断档 | 468.5 ms | 517.5 ms |
| 最终 A/V skew | +66.9 ms | +58.2 ms |
| 最大视频帧龄 | 1735 ms | 2040.5 ms |

50 ms 的最终 skew 与 >100 ms 间隔数方向略好，但空转、播放丢块、包间最大间隔和设备最大断档变差；每档仅两轮，**没有足够证据扩大 20 ms 默认等待**。两档各两轮都达到 1.2 秒以上累计空转，设备断档最高 683 ms；原有弱网 PASS 只验视频/协议门槛，不能代表音频连续性达标。

四轮共记录 33 个超过 100 ms 的音频包间隔，其中 5 个发生在某个 IDR 到达查看端前后 100 ms 内；只有一组时间戳完全重合（音频间隔 269.3 ms、IDR 到达同为 10212.4 ms，该 IDR 提交到接收约 358.2 ms）。其余长间隔并未普遍紧邻 IDR。该时间相关性值得后续观察，但本机模拟、少量事件和网络队列混杂不足以证明 IDR 导致音频断续。

新增仅供探针显式启用的连续性预算参数：`--audio-max-starved-ms`、`--audio-max-device-gap-ms`、`--audio-max-dropped-blocks`、`--audio-max-abs-av-skew-ms`。可按实验目标单独指定一个或多个限制；超限会使 `connect`/`verify --netem` 失败，要求的 A/V skew 不可用时也判失败。预算默认全部关闭，不改变产品播放策略及既有 CTest 行为。参数边界的确定性测试加入 `audio_playout`；有预算的弱网探针另行核对连续性与 3000 ms 视频帧龄。当前尚未基于产品体验确定正式门槛，示例预算只能用于实验，不能当成已达标标准。

预算验证二进制 SHA-256：`8EB37FC34ABBD60B274EE4338CA2DE56190C1C06DD81C1A9B7649DF26F242DEA`。`audio_playout` CTest 通过；超范围的空转/非有限 skew 参数以退出码 2 拒绝。25 秒弱网探针设置空转预算 1000 ms 时实测空转 880 ms、设备最大断档 368 ms、丢块 30、skew +75.8 ms、最大视频帧龄 1969 ms，门禁关闭项下原有弱网门禁 PASS。另一次同条件探针只把空转预算改为 500 ms，实测空转 986 ms、设备最大断档 369 ms、丢块 45、skew +48.8 ms、最大视频帧龄 2310 ms；整体验证按预期 FAILED，尽管原有 3000 ms 视频帧龄门槛仍通过。这验证超限会令探针失败，也显示连续性指标存在明显单轮波动；它们不是产品验收门槛结论。

**本机结论：** 20/50 ms 顺序对照没有解决明显断续，且这一实现之前已经由另一个模型完成；暂不调整产品默认值。当前应使用可失败的显式预算继续评估接收端迟到接受与发送后共享带宽排队，并同时观察音频连续性和视频时效。单机结果不替代真实跨网验收。

#### 2026-11-21 C03 播放迟到块保留与有界容忍对照

为直接验证 RustDesk FIFO 路径与当前按时间戳丢迟到块路径的取舍，在 `chast-quic` 增加探针专用 `--audio-keep-late-blocks`。它把迟到截止容忍设为 `UINT32_MAX`，有效关闭迟到丢块；接收乱序等待、500 ms 播放队列上限及停顿后清队列/重新锚定保持不变，因此这不是不受限 PCM FIFO，也不是产品设置。`audio_playout` 确定性测试覆盖极大容忍值及边界不回绕。实验二进制 SHA-256 为 `F23FDCF3622A3AA802DD65B4F03A53A188649AA003DE6FAE48A63157186B8591`。

先按 20 ms / 保留迟到块 / 保留迟到块 / 20 ms 做四轮 ABBA；再按 20/40/40/20 ms 做有界容忍 ABBA。每轮 25 秒，均为静音 WASAPI 播放、10 ms 端点请求、自适应 jitter 与到达相位修正、20 ms 音频重排等待；弱网参数为 80 ms 基础延迟、0–20 ms 抖动、3% 丢包、1000 kbps，4–12 秒劣化。相同二进制和参数下，迟到保留对照四轮、20/40 ms 对照四轮原始日志均在 `build/test-temp/c03-audio-late-block-retention-abba-*-20261121.log`、`build/test-temp/c03-audio-late-tolerance-20-40-abba-*-20261121.log`。实验没有设置音频连续性预算；弱网 `PASS` 只表示原有传输与视频门禁通过，不等价于音频达标。比较表采用中位数和范围；各档仅两轮，单机环回且不代表跨网听感：

| 播放迟到容忍 | 累计播放空转中位数（范围，ms） | 丢弃块中位数（范围） | 设备最大断档中位数（范围，ms） | A/V skew 中位数（范围，ms） | 音频最大到达间隔中位数（范围，ms） | 最大视频帧龄中位数（范围，ms） | 视频门禁 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 20 ms 基线（同组 ABBA） | 1714.5（1077–2352） | 109（44–174） | 1234（390–2078） | +74.1（+67.7–+80.4） | 195.2（188.4–202.0） | 2448（1936–2960） | 2/2 |
| 40 ms | 1475.5（1450–1501） | 85（74–96） | 501.5（472–531） | +88.7（+65.5–+111.8） | 246.1（237.4–254.8） | 1570（1136–2005） | 2/2 |
| 60 ms（仅通过门禁的单轮） | 1138 | 57 | 400 | +101.4 | 216.7 | 1655 | 1/2 |
| 保留迟到块（近似无限容忍） | 835（783–887） | 0 | 95.5（87–104） | +271.0（+268.8–+273.1） | 197.1（195.4–198.7） | 1256（1058–1455） | 2/2 |

门禁通过的 20 ms 与 60 ms 各只有一轮；该 60 ms 样本显示空转、丢块和设备最大断档下降，但 A/V skew 增加，不能支持默认改为 60 ms。40 ms 两轮均通过现有 3000 ms 视频帧龄门禁；与同组 20 ms 基线相比，中位空转下降约 14%、丢块下降约 22%、设备最大断档下降约 59%，skew 中位增加约 15 ms。然而两轮 40 ms 的最大音频到达间隔都比两轮 20 ms 样本更长，且样本很少，改善也没有在全部指标上同步出现。保留迟到块时丢弃为零、设备最大断档明显下降，但播放队列占用升至 230–240 ms，A/V skew 恶化到约 +270 ms，故不能照搬 FIFO 容忍策略。

20/60 对照里，20 ms 和 60 ms 各有一轮最大视频帧龄超过 3000 ms（分别 4107 ms、3938 ms），另各一轮通过。超限均发生在 IDR/共享链路排队明显的样本；不能将失败单独归因于音频容忍档位，也不能把失败轮从原始证据中删除。20/40 四轮均通过，但其中基线一轮最大帧龄为 2960 ms，接近门槛。以上仍是同一台机器的环回弱网模拟，播放空转和帧龄有显著单轮波动。

**决策：** 保留产品 20 ms 默认值；不采纳近似无限迟到容忍。40 ms 是值得进一步重复验证的候选实验值，不是已批准的优化。下一轮先在同一最终二进制复做 20/40 ms 交替样本，并同时设置连续性预算、A/V skew 与视频帧龄门禁；若 40 ms 改善不能跨轮复现或使 skew 超界，就不扩大默认容忍。持续查明 QUIC 提交后的共享带宽/IDR 排队，因为改变接收播放截止时间不能缩短音频包的实际到达间隔。不得以该单机探针结果宣称真实跨网音频合格。

#### 2026-11-21 C03 播放器阶段计时与重复对照

在同一 Release 探针上加入只读 WASAPI 播放阶段计时后，按 20/40/40/20 ms 再做四轮 ABBA。二进制 SHA-256 `68B9312D156A5BA399124B42D386EFF193EB1D332656B55E5F633DC904A9955A`；四轮使用相同二进制、静音 WASAPI 输出、10 ms 端点请求、自适应 jitter/到达相位修正、20 ms 重排等待。弱网参数仍为 80 ms 基础延迟、0–20 ms 抖动、3% 丢包、1000 kbps 限速、4–12 秒劣化窗口。每轮启用仅供实验的连续性预算：累计播放器空转 ≤2000 ms、最大设备断档 ≤1000 ms、丢弃 ≤200 块、绝对 A/V skew ≤120 ms；视频最大帧龄门槛为 3000 ms。原始日志：`build/test-temp/c03-audio-render-stage-20-40-abba-20a-20261121.log`、`c03-audio-render-stage-20-40-abba-40a-20261121.log`、`c03-audio-render-stage-20-40-abba-40b-20261121.log`、`c03-audio-render-stage-20-40-abba-20b-20261121.log`。指标只来自这一台机器的 loopback/受控弱网模拟。

| 指标（每档两轮中位数，括号内为范围） | 20 ms | 40 ms |
| --- | ---: | ---: |
| 播放器累计空转 | 1369.5 ms（1196–1543） | 1425 ms（1176–1674） |
| 设备时间线累计空转 | 2214.5 ms（2096–2333） | 2369.5 ms（2260–2479） |
| 设备最大断档 | 708 ms（688–728） | 520.5 ms（278–763） |
| 播放器输入最大间隔 / 音频包最大到达间隔 | 336 ms（171–501） | 292 ms（289–295） |
| 入播放器队列至首次写入 WASAPI 最大等待 | 302 ms（201–403） | 418 ms（395–441） |
| 队列有音频时渲染唤醒最大间隔 | 11 ms（11–11） | 11 ms（11–11） |
| 丢弃块 | 77.5（67–88） | 69.5（42–97） |
| A/V skew | +57 ms（+52–+62） | +79.7 ms（+78–+81.3） |
| 最大视频帧龄 | 1138 ms（1039–1237） | 1574.5 ms（1261–1888） |
| 连续性预算与视频帧龄门槛 | 2/2 通过 | 2/2 通过 |

四轮均无渲染调用失败、端点重开或播放器/接收队列溢出。`render-wake-max-gap` 始终为 11 ms，而 WASAPI 设备最大断档为 278–763 ms；播放器输入最大间隔又与音频包到达最大间隔一致（171–501 ms）。主机 Opus 提交最大间隔为 29–34 ms、采集至提交年龄为 35–42 ms；MsQuic 提交至成帧最大值为 95.7–232.8 ms，且没有与每次接收长间隔稳定对应。因此当前证据把故障方向收窄到 WASAPI 渲染唤醒之外、播放器收到连续音频输入之前的发送/传输/链路/接收路径，并提示输入入队后到首次设备写入仍可能有 201–441 ms 等待；它还不能区分网络排队、数据报服务/接收调度和预定播放槽等待，也不能证明某一个 IDR 导致断续。

40 ms 的设备最大断档与丢块中位数低于 20 ms，但累计空转略高，A/V skew 约多 23 ms，且队列入设备等待显著更高；各档仅两轮，指标取舍与波动仍不支持更改产品默认值。四轮通过的是实验预算而非已按听感或产品需求批准的验收标准；单机结果不等价于真实跨网表现。下一步针对发送提交、QUIC 成帧、接收入队、重排释放和预定播放槽等待增加可关联的同机事件轨迹，检查长间隔的时间对应关系；确认是接收前缺包还是入队后等待，再分别评估发送调度/共享链路竞争与播放策略。保持默认 20 ms，不扩张至 RustDesk 的 3000 ms PCM 容量，也不把当前预算 PASS 宣称为音频连续性达标。

#### 2026-11-21 C03 按秒播放轨迹复核

启用探针现有 `--audio-playout-trace`，仍用上节 Release 二进制 SHA-256 `68B9312D156A5BA399124B42D386EFF193EB1D332656B55E5F633DC904A9955A` 和相同 25 秒弱网参数/连续性预算，另跑 20、40、100 ms 各一轮。新增日志：`build/test-temp/c03-audio-stage-trace-20ms-20261121.log`、`c03-audio-stage-trace-40ms-20261121.log`、`c03-audio-stage-trace-100ms-20261121.log`。跟踪字段为会话累计最大值，按相邻秒输出；`audio-gap` 使用同机 steady-clock 相对时间，可以和 `audio-trace t=` 对齐。

三轮的音频接收轮询最大间隔为 12.0–12.5 ms，解码至播放器 push 最大耗时为 0.3–0.5 ms，视频解码最大耗时为 34.7–39.6 ms；未发现这三个本机处理阶段出现秒级停顿。三轮接收端最长数据报间隔分别为 188.3、207.5、191.1 ms，最大 QUIC 接收队列驻留 49.7、73.7、49.2 ms，均无接收队列溢出。20 ms 样本中约 t=4–5 秒播放器迟到丢弃由 9 增至 71，设备最大断档升至 622 ms；40 ms 样本在约 t=4–6 秒丢弃由 24 增至 160，设备最大断档升至 1704 ms，累计播放空转达 2000 ms 后继续升至 2201 ms。100 ms 样本约 t=4–5 秒迟到丢弃升至 81，设备最大断档升至 863 ms。此时间关系与截止丢弃/播放锚点处理参与放大断档相符，但累计计数和一秒采样只能说明时间相关，不能单独证明具体控制逻辑是唯一原因。

| 迟到容忍 | 播放空转 | 最大设备断档 | 迟到丢弃 | A/V skew | 最大视频帧龄 | 本轮门槛 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 20 ms | 1382 ms | 622 ms | 89 | +76.3 ms | 1609 ms | 通过 |
| 40 ms | 2201 ms | 1704 ms | 178 | +101.2 ms | 1603 ms | 音频预算失败 |
| 100 ms | 1464 ms | 863 ms | 102 | +152.2 ms | 935 ms | A/V skew 预算失败 |

该小样本中，20 ms 达到本轮人为设置的预算；40 ms 的空转和设备断档均越限；100 ms 虽未越过空转/设备断档预算，A/V skew 却超过 120 ms。三轮都未越过 3000 ms 视频帧龄门槛。不能把某档单轮表现推广到产品体验，也不能以此断言链路没有问题；但它不支持将默认值扩大到 40 或 100 ms。**暂时维持 20 ms，不合入容忍值变更。** 下一步需要采集逐包或逐次播放事件的时间戳，至少记录序号、接收回调、重排释放、入队、迟到丢弃与首次 WASAPI 提交，并与同一单调时钟上的设备断档配对；当前按秒累计最大值不足以证明单个音频块的因果链。

#### 2026-11-21 C03 逐包播放事件轨迹修正

为补齐逐包关联，先修正探针中的 steady-clock 目标时间单位换算；此前把微秒起点又乘了 1000，导致输出负时间。播放事件改为按类型分别限额（late-drop 256、first-write 128、device-gap 128），late-drop 只记录迟到至少 50 ms 的严重样本，避免高频事件挤掉设备断档。复核又发现 device-gap 探针在更新设备结束时间后才读取“上一个结束时间”，使该事件永远无法满足触发条件；现改为先取旧结束时间再更新统计。PLC 合成块现在带缺失序号、`concealed` 标记、揭示缺口的后续包序号及该包接收/重排时刻，避免与真实序号 0 混淆。以上均为诊断元数据/轨迹修正，不改变默认播放策略。

Release `chast-quic` SHA-256：`06B6547E7FE43AAB8216EFFF447F2F1260CC258625B067D445191B6D56618C28`。同一 25 秒单机受控弱网参数（80 ms 基础延迟、0–20 ms 抖动、3% 丢包、1000 kbps、4–12 秒劣化、20 ms 重排等待、20 ms 迟到容忍、10 ms WASAPI 请求缓冲、自适应 jitter 与到达相位修正）复跑一次。日志：`build/test-temp/c03-audio-packet-trace-20ms-20261121-rerun2.log`。

| 指标 | 本轮结果 |
| --- | ---: |
| 播放器累计空转 | 1279 ms |
| 设备时间线累计空转 | 2015 ms |
| 设备最大断档 | 669 ms |
| 音频包最大到达间隔 | 254.7 ms（6 次超过 100 ms） |
| 接收队列最大驻留 | 51.7 ms，峰值 6 包 |
| 播放器输入最大间隔 / 入队至首次设备写入最大等待 | 255 ms / 404 ms |
| 渲染唤醒最大间隔 | 11 ms |
| 丢弃 / A/V skew / 最大视频帧龄 | 82 块 / +64.8 ms / 2290 ms |
| 端点重开 / 渲染失败 | 0 / 0 |

日志共保留 6 条 device-gap、49 条严重 late-drop 和 128 条 first-write 事件；first-write 达到该类型的上限，较早事件会由新事件替换。没有负值目标时间。最大断档附近，接收包间隔为 `seq=376->377` 的 131.4 ms，而 `seq=377` 对应设备断档为 669.2 ms；两者时间重合但数值不同，说明设备断档不是单个相邻包间隔的同义指标，还跨过了播放时间线上的迟到/丢弃区间。PLC 事件现可按缺失序号和 `revealed-by` 与揭示缺口的真实包关联。该单轮临时预算内通过，但空转、断档与丢弃仍显著；这既不证明音频体验达标，也不证明具体根因已经唯一确定。

`chast-quic`、`chast-audio-playout-test`、`chast-viewer-probe` Release 构建通过，保留既有 FFmpeg `C4244` 与链接 `LNK4098` 告警。定向 CTest 最近一轮 5/6 通过：`audio`、`audio_dsp`、`audio_playout`、`viewer_audio_video_isolation`、`quic_weak_network` 通过；`protocol` 每次都在既有物理锁键注入断言失败（“a physical lock key is never synchronized with itself”），与本次音频变更无关，尚未处理。为使 `chast-protocol-test` 可独立链接，`chast.net` 的公开依赖补上其 `src/win/input.cc` 调用的 `imm32`。`git diff --check` 通过。

**该轮状态：** 逐包轨迹展示了接收、迟到丢弃、首次设备写入和设备断档的同机时间关系；当时只有一轮复测，且 `first-write` 事件达到容量上限。后续重锚积压及多轮保留队列对照见下节。产品默认仍为 20 ms，不采用 RustDesk 式 3000 ms PCM 容量；单机模拟预算 PASS 不代表产品音频 PASS。

#### 2026-11-21 C03 重锚积压 PCM 计数与保留队列对照

逐包样本确认到达间隔超过 100 ms 时，播放器会重锚；原策略无条件清空 PCM 队列。部分重锚事件清除了 78–158 ms 积压；另有事件在队列中仍有 350 ms PCM 时重锚。旧统计未将重锚清除计入 dropped，因而低估实际丢弃。本轮修复音频事件种类计数数组只有三项、而事件枚举已有四项的越界缺陷，并将重锚丢弃块数和剩余 PCM 时长单独统计，同时计入播放器 dropped。探针新增对照选项 `--audio-preserve-reanchor-queue`，可单独复现该策略；查看端系统声音在五轮对照后显式启用，未改变通用播放器和麦克风播放默认值。

使用同一个 Release `chast-quic`（SHA-256 `61C9C71AC5D5EB1D93690E9A57924FC8352733636FB0FD9B69F9B44EA8B87BB7`）运行五轮默认策略与五轮保留队列策略。所有有效样本均为 25 秒、静音 WASAPI 播放、10 ms 请求缓冲（当前端点实际 22 ms）、6 块启动收集、20 ms 重排等待和迟到容忍、自适应 jitter 与到达相位修正；弱网参数为 80 ms 延迟、0–20 ms 抖动、3% 随机丢包、1000 kbps 限速、4–12 秒劣化窗口。每轮使用相同实验预算：累计空转 ≤2000 ms、设备最大断档 ≤1000 ms、播放丢弃 ≤200 块、绝对 A/V skew ≤120 ms；视频帧龄仍以 3000 ms 为上限。运行顺序为默认、保留、保留、默认、保留、默认、默认、保留、保留、默认；指标为五轮中位数和范围：

| 指标 | 清空队列（实验基线） | 重锚时保留队列（查看端策略） |
| --- | ---: | ---: |
| 播放器累计空转 | 1817 ms（892–1996） | 1238 ms（850–1855） |
| 播放丢弃块 | 171（74–178） | 85（63–162） |
| 重锚清除 PCM | 35 块 / 350 ms（15–52 块 / 148–512 ms） | 0 |
| 设备最大断档 | 731 ms（428–1187） | 504 ms（287–1050） |
| A/V skew | +62.6 ms（+48.7–+77.1） | +73.9 ms（+42.3–+84.0） |
| 最大视频帧龄 | 1866 ms（1028–2279） | 1467 ms（1050–2094） |
| 连续性预算及原有弱网门禁 | 4/5 通过 | 4/5 通过 |

两组都各有一轮因设备最大断档超限而失败；其余指定音频预算和 3000 ms 视频帧龄在十轮中均通过。重锚保留组的空转、丢弃和设备断档中位数较低，A/V skew 中位数则增加约 11 ms，仍低于 120 ms 预算。五轮结果支持将该策略限定应用于查看端系统声音：在接收停顿后的新 burst 到来时保留可能仍可播放的 PCM，让现有迟到截止和 500 ms 队列上限继续裁剪过期或过量数据。现已只在查看端系统声音播放器显式启用，麦克风播放及通用播放器默认不变。该改动不是 C03 完成结论：每种策略仍各有一轮音频预算失败，数据来自单机模拟，不能替代真实网络听感或跨网验收。

有效对照日志：`build/test-temp/c03-reanchor-queue-abba-20ms-a-20261121.log`、`c03-reanchor-queue-abba-preserve-a-20261121.log`、`c03-reanchor-queue-abba-preserve-b-20261121.log`、`c03-reanchor-queue-abba-20ms-b-20261121.log`、`c03-reanchor-queue-abba-preserve-c-20261121.log`、`c03-reanchor-queue-abba-20ms-c-20261121.log`、`c03-reanchor-queue-abba-20ms-d-20261121.log`、`c03-reanchor-queue-abba-preserve-d-20261121.log`、`c03-reanchor-queue-abba-preserve-e-20261121.log`、`c03-reanchor-queue-abba-20ms-e-20261121.log`。另有一轮早期检查使用了 120 ms 默认端点缓冲且未启用 jitter/相位修正，参数与对照不符，已排除、不纳入上表。查看端策略变更后已重建 `chast-viewer-probe`（SHA-256 `E7F405CA0113E5DB82CA0CACA7AC5ADCE3F1DB0A38640D2592599B7039368CEA`）和 `chast-quic`（SHA-256 `F222BAF4AB4D061FE696DFDDB4F428195FE10AA8A637683502B71683458D01F3`）；定向 CTest `audio`、`audio_dsp`、`audio_playout`、`viewer_audio_video_isolation`、`quic_weak_network` 5/5 通过。构建保留已有 FFmpeg `C4244` 和链接 `LNK4098` 告警，`git diff --check` 通过。

#### 2026-11-21 C03 查看端策略落地后的干净/弱网交替复验

启用查看端系统声音重锚保留策略后，已用上节 `F222BAF4AB4D061FE696DFDDB4F428195FE10AA8A637683502B71683458D01F3` 构建完成干净链路、弱网各五轮交替复验，每轮 25 秒。使用 10 ms WASAPI 请求缓冲（本机实际 22 ms）、6 块启动收集、20 ms 重排等待和迟到容忍、自适应 jitter、到达相位修正及 `--audio-preserve-reanchor-queue`。弱网配置仍为 80 ms 延迟、0–20 ms 抖动、3% 随机丢包、1000 kbps 和 4–12 秒劣化窗口。沿用空转 ≤2000 ms、设备最大断档 ≤1000 ms、丢弃 ≤200 块、绝对 skew ≤120 ms 的实验预算；弱网视频帧龄上限为 3000 ms。

| 指标（五轮中位数，括号内为范围） | 干净链路 | 弱网模拟 |
| --- | ---: | ---: |
| 播放器累计空转 | 349 ms（322–446） | 1594 ms（1306–1909） |
| 设备最大断档 | 264 ms（255–328） | 522 ms（459–694） |
| 播放丢弃 | 21 块（19–25） | 137 块（106–154） |
| 最终 A/V skew | +68.6 ms（+49.2–+80.1） | +57.4 ms（+38.9–+65.7） |
| 最大视频帧龄 | 27.2 ms（24.4–373.2） | 1286 ms（1154–1951，按弱网汇总取整） |
| 整体验证 | **4/5 通过** | **5/5 通过** |

十轮播放指标均在上述实验预算内，无重锚清队列丢弃、播放端点重开或渲染调用失败。干净第 1 轮整体验证仍因主机音频路径总丢弃 3 个而失败，日志为 `c03-final-reanchor-clean-1-20261121.log`；不能把该组写成全通过。当时统计未拆分采集与发送丢弃，因此尚不能判断这 3 个丢弃发生在哪一侧。干净五轮最终 skew 均高于既有稳定链路 ±40 ms 目标，120 ms 实验预算通过不能替代该目标或听感验收。弱网五轮通过也只是这组单机受控条件下的结果，仍有累计空转及设备断档。

完整本机日志：`build/test-temp/c03-final-reanchor-clean-{1..5}-20261121.log` 和 `build/test-temp/c03-final-reanchor-weak-{1..5}-20261121.log`。这些十轮证据均早于下节丢弃分项统计的新构建；不得将它们标成新二进制的复验结果。

#### 2026-11-21 C03 主机丢弃归因与本次备份检查

为定位干净链路失败样本，将主机 `audio_dropped` 拆为 `audio_capture_dropped` 和 `audio_send_dropped`，总数继续保留，并仅记录最先三次发送丢弃的具体错误；干净/弱网探针汇总同时输出两项。该变化增加归因能力，尚未证明丢弃根因修复。最新 Release 构建通过，日志为 `build/test-temp/c03-host-drop-split-build-20261121.log`，保留既有 FFmpeg `C4244` 与链接 `LNK4098` 告警。

| 当前构建 | SHA-256 |
| --- | --- |
| `chast-quic` | `E95D483B97DC787C3A417E5E45EAA86E9ADAC0BE7F35959693521C033E7FAD79` |
| `chast-viewer-probe` | `BADC606263DB1DD761453E505D8F40038EAD2F90EFA3BA4E3E21408BD1A7A883` |

新构建的专门干净链路复测在用户暂停时中断；`build/test-temp/c03-audio-drop-attribution-clean-20261121.log` 只有前段检查，没有完成结果，不计入有效通过/失败轮次。临时产物 `.chast-quic-verify-38658307018900.bin` 保留在本机，未纳入提交。本次备份只更新状态文档并归档已有源码改动，没有继续调整产品音频参数或重新进行多轮音频实验。

2026-11-21 本次提交前重新运行 `ctest --test-dir build -C Release -R '^(audio|audio_dsp|audio_playout|viewer_audio_video_isolation|quic_weak_network)$' --output-on-failure`，**5/5 通过，37.68 秒**；日志 `build/test-temp/c03-status-backup-ctest-20261121.log`。其中 `quic_weak_network` 不启用 WASAPI 音频播放，不能替代音频连续性复测。`protocol` 历史锁键断言失败仍待排查，本次未重跑；Go、Flutter 全量与正式服务门禁也未在此次原生音频状态备份中重跑，既有结果按原日期保留。提交前 `git -c core.whitespace=cr-at-eol diff --check` 通过。

**下一步（C03 未关闭）：**

1. 用上述带采集/发送丢弃分项的当前构建复测干净链路，区分采集队列、发送拒绝或路径预算，定位主机 3 个丢弃样本；中断日志不纳入统计。
2. 修复归因后，在同一最终构建下重新完成干净/弱网交替多轮门禁，同时复核连续性、稳定链路 skew、播放延迟、帧率和画质，再扩展劣化强度及持续时间。维持当前 20 ms 默认容忍与 500 ms PCM 上限，不直接采纳扩大容忍或视频窗口实验。
3. 排查历史 `protocol` 物理锁键断言失败，并在后续发布验收中补齐完整门禁。
4. 有第二台设备后完成真实跨网、可懂语音、下行参考下的 AEC/降噪、物理音频设备切换及长时听感验收；实验预算 PASS 不升级为产品音频 PASS。

外部验收准备继续保留：

1. 准备第二台可连接 Windows 实机；记录显示器数量、DPI/缩放、网络拓扑和连接方式，不记录密码。
2. 验证真实 Flutter 键鼠、跨机文件传输、双物理机管理台断开、断网/重连、跨 NAT/代理和较长故障窗口。
3. 单机 loopback 的成功结果保持为单机证据，不升级为双机或跨网结论。

### P3：进入 W2

按 2026-11-20 用户决定，双机验证后置；W1 外部验收尚未收口。C01 真实电源行为、多会话实机释放，C02 混合 DPI、旋转、全屏和真实多屏场景继续待验。C03 媒体按钮、共享状态同步、自适应播放、音视频线程隔离、接收重排、查看端重锚保留 PCM 和诊断随本次归档。最近干净/弱网各五轮交替复验已完成，弱网 5/5 达到实验预算，干净 4/5 且稳定链路 skew 未达既有 ±40 ms 目标；新增主机丢弃分项后仍需定位并重验，C03 不关闭。之后以可控语音、下行参考验证可懂度与 AEC/降噪，补真实窗口控制、显示场景和物理热插拔，再实现多连接标签页、H03 跨重启队列和 C04。账号隔离、越权、数据丢失、错误断开和核心连接阻断问题仍优先。

## 6. 当前门禁与提交前检查

本轮提交前应执行并记录：

```powershell
# Go
cd D:\ds-chcast\server
go test ./...
go vet ./...

# Flutter（在 D:\ds-chcast\app）
flutter analyze
flutter test

# 管理台 JavaScript
node --test server/internal/webadmin/app_session_test.cjs
node --check server/internal/webadmin/native_disconnect_browser.cjs
```

2026-11-19 本轮完整门禁复跑结果：

| 门禁 | 结果 |
| --- | --- |
| `cd server; go test ./...` | PASS |
| `cd server; go vet ./...` | PASS |
| `cd app; flutter analyze` | PASS |
| `cd app; flutter test` | PASS，815/815 |
| `node --test server/internal/webadmin/app_session_test.cjs` | PASS，48/48 |
| `node --check server/internal/webadmin/native_disconnect_browser.cjs` | PASS |
| Flutter Windows Release 构建 | 最新复建 PASS，40.3 秒；保留 `UNICODE`/`_UNICODE` 重定义、`getenv` 安全提示及 `LNK4098` 默认库冲突告警 |
| P1 定向 CTest | 最新复验（2026-11-20，路径匹配和进程句柄回归修正后）：18 项中 17 项 PASS、`machine_state_security_untrusted_owner` 因错误 1300 单独 Skipped，188.22 秒；含运行中幂等/配置变更策略、安装/卸载 fake 事务、进程超时清理、Firewall COM 内存对象配置及 ACL 基础测试，`host_parity` 123.63 秒通过。 |
| `tools/test-service-scm.ps1` Parser 与默认只读预检 | PASS；无服务、配置文件或产品防火墙规则 |
| `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -InputInjection` | PASS，晚间复跑 12.065 秒；A-up 索引 11、清理边界 15，无重复远端 A 事件 |
| PowerShell Parser：`tools/test-native-runtime.ps1` | PASS |
| PowerShell Parser：`tools/native-flutter-input.ps1` | PASS |
| PowerShell Parser：`tools/native-flutter-file-transfer.ps1` | PASS |
| `git -c core.whitespace=cr-at-eol diff --check` | PASS；保留 `viewer_bridge.cc` 原有混合换行 |

本轮 `go test ./...`、`go vet ./...`、Flutter analyze/test、管理台 JavaScript 测试/语法检查及 Flutter Release Windows 构建均已复跑通过；原生 Flutter 输入轨迹也在晚间重新 PASS。ProgramData ACL 加固代码已实现并通过隔离目录验证，但非可信 owner 分支尚未覆盖；该专项因当前令牌缺少可用的 `SeRestorePrivilege` 而单独 Skipped。修正前 `--token` 曾尝试创建该目录但被拒绝，复核确认没有创建或修改；修正后仅通过临时目录信任检查回归，没有对真实 ProgramData 再运行命令。未执行的 PostgreSQL 生产拓扑、正式 SCM 生命周期、真实 COM 回滚、第二台 Windows、跨网和性能对照继续标记待验收，不能用单机结果替代。

## 7. 证据索引

- 最新 Flutter 键鼠释放 PASS：`build/native-runtime-20261119-225354-3f006de1/run-657812140/`
- 2026-11-20 Flutter 键鼠释放复验 PASS：`build/native-runtime-20261120-020330-0137235e/run-1594865917/`
- 较早 Flutter 键鼠释放 PASS：`build/native-runtime-20261119-203601-9bc48695/run-1423269566/`
- 较早 Flutter 键鼠释放 PASS：`build/native-runtime-20261119-180114-107055db/run-2803120376/`
- 修复前的键鼠失败证据：`build/native-runtime-20261119-001020-ef1059e1/run-2160080674/`
- 2026-11-18 单机组合验收：`build/native-runtime-20261118-214914-882e8a81/`
- 2026-11-18 双 Flutter/文件/管理台断开：`build/native-runtime-20261118-215415-dbfc4d4a/run-369350478/`
- 2026-11-18 viewer 策略三阶段：`build/native-runtime-20261118-222855-69bd0658/`
- 临时 SYSTEM 服务：`build/service-manager-startup-system.log`
- 详细原生验收：[native-runtime-acceptance-2026-11-17.md](native-runtime-acceptance-2026-11-17.md)
