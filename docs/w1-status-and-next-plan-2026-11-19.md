# W1 当前状态、完成标记与后续计划

日期：2026-11-20

分支：`codex/s03-role-console-2026-11-17`

当前基线：`2c3ff5c`（本轮验收改动尚未提交）

适用范围：Windows 单机验收、W1 企业管理主干收尾。本文是当前 W1 状态入口；历史文档中的中间结论如与本文冲突，以本文和对应原始证据为准。

## 1. 总体结论

W1 尚未完成，当前不能宣称发布候选通过。已经完成的部分集中在 W1 企业管理主干、单机原生进程、管理台断开、Flutter 视频/文件链路、Flutter 窗口键鼠释放序列和临时 SYSTEM 服务监督验收；仍缺口主要是第二台 Windows 实机、跨网、正式 SCM 服务及生产环境。

当前证据等级分为三类：

- **已完成**：有自动回归或真实进程产物，能直接支撑对应结论。
- **单机已验证**：只在 loopback、本机模拟输入或临时服务身份下通过，不能替代双机、跨网或正式服务。
- **待验收**：代码或工具已经存在，但没有达到完成所需的真实环境证据。

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

1. 准备第二台可连接 Windows 实机；记录显示器数量、DPI/缩放、网络拓扑和连接方式，不记录密码。
2. 验证真实 Flutter 键鼠、跨机文件传输、双物理机管理台断开、断网/重连、跨 NAT/代理和较长故障窗口。
3. 单机 loopback 的成功结果保持为单机证据，不升级为双机或跨网结论。

### P3：进入 W2

按 2026-11-20 用户决定，双机验证后置；W1 外部验收尚未收口。本机可验证的 C01/C02 代码和自动化门禁继续推进，但不代表发布验收完成。C01 真实电源行为、多会话实机释放仍待验；C02 混合 DPI、旋转、全屏及真实多屏场景待验。C03 已实现初次布局和本机拓扑变化时的离屏恢复，native 双引擎及同进程双 Flutter 窗口连接隔离均有单机回归通过；下一步验证逐窗口录制/音频控制 UI 与真实音频负载，并在获准时进行物理热插拔验收，再评估多连接标签页。双机验收仍后置。账号隔离、越权、数据丢失、错误断开和核心连接阻断问题仍优先于功能扩展。

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
