# H01 便携特权辅助服务设计

更新日期：2026-11-26
分支：`codex/complete-workspace-2026-11-20`
状态：设计基线、IPC framing、生命周期协调器、Win32 SCM adapter、独立 SCM 服务入口、受保护暂存/copy API、持久恢复记录、正式服务后台扫描、经身份校验的 Status/Stop 控制通道、本地 capability/WorkerReady 策略核心与注入式 session-worker 启动/握手协调器已实现；按连接派生的 session-worker Named Pipe endpoint 已实现。session-worker launcher 尚未接入正式 helper service 控制循环，生产 token/session 启动策略和桌面 worker 未完成；发布者固定机制已实现，但项目尚无签名证书/指纹；UAC、真实 SCM 验收及桌面捕获/输入桥接仍待完成

2026-11-26 SCM 控制通道与 session-worker 启动协调：`PortableHelperServiceLifecycle::CreateAndStart` 在 SCM 启动后要求建立已认证控制通道，失败会回滚本次拥有的临时服务；停止先请求控制通道 Stop，再走 SCM Stop/Delete。正式 helper entry 持续校验父进程身份，只提供 Status/Stop，并在确认 Stop 后退出服务。独立 session-worker 协调器负责创建连接专属 endpoint、调用外部 launcher、核验进程身份与 session、认证 pipe peer、发送 CapabilityGrant 并验证 WorkerReady；启动中任一步失败都会关闭通道、撤销待处理授权并尝试有界终止进程，终止失败时保留句柄供调用方清理。Release `chast-host-parity-test` 和 `chast-portable-helper-lifecycle-test` 均构建成功；`portable_helper_authorization` 与 `portable_helper_lifecycle` CTest **2/2 PASS**。回归 launcher 是隔离普通本机子进程，Win32 进程句柄/pipe 撤权路径也不代表 SCM SYSTEM worker；launcher 由调用方注入且尚未接入服务控制循环，没有用户 token/session 选择策略、UAC 授权调用方或真实桌面 capture/input adapter。未安装/启动真实服务、触发 UAC、写 ProgramData/防火墙或注入输入。后续先接通产品控制流程与明确的 token/session 设计，再安排隔离管理员真实 SCM 验收。

2026-11-26 H01 session-worker IPC endpoint 切片：`PortableHelperPipe` 新增 `CreateSessionWorkerServer` / `ConnectSessionWorkerClient`，endpoint 由 helper instance ID 与非零 connection ID 派生，framing 继续绑定 helper instance ID；控制 endpoint 与 session worker endpoint 可并行存在且各自独占。Release `chast-host-parity-test` 构建成功，直接运行 `--portable-helper-authorization` 通过；隔离本机两个子进程分别连接控制和 worker endpoint，并验证精确 PID 身份、独立双向帧序号/请求往返以及错误 connection ID 拒绝。此为真实本机 Named Pipe 传输证据，不是 SYSTEM/SCM worker 验收；endpoint 尚未接入 `run_portable_helper_service_control_channel`、服务进程启动流程或桌面回调，未创建服务、触发 UAC、修改 ProgramData/防火墙或注入真实输入。下一步应实现服务侧 session-worker 启动与两管道协调，并让 worker 完成 grant/WorkerReady 和有界撤权；随后再将真实 capture/input adapter 接入，并保持真实 SCM 验收后置。

2026-11-26 capability 与桌面请求门禁切片：新增 `host/portable_helper_authorization.{h,cc}`，以 BCrypt 生成每次授权独立的 secret/challenge，WorkerReady proof 使用 HMAC-SHA256 绑定 helper 实例、连接 ID、Windows session、代次和操作掩码；WorkerReady 明确要求预期桌面 worker 的 session 等于 grant 目标 session，且 PID、创建时间、映像、用户 SID、logon SID 全部匹配和 proof 正确，状态才从 AwaitingWorkerReady 进入 Ready。新增严格类型化的 `CaptureFrameRequest` / `InputEventRequest`，输入体复用现有 24 字节 InputEvent schema，固定绑定连接/代次/session，禁止通用命令；操作 gate 核验每条请求，返回的 RAII lease 必须覆盖捕获/注入副作用。撤权提供 Begin/Wait/Finish 拆分：先关闭新请求并清除 capability、调用取消回调；监督者可限时等待在途 lease，超时后强制终止 worker，再等待 lease 排空并释放输入。`Revoke` 便利 API 保持阻塞排空后释放输入的行为；取消/释放失败仍保持撤权并要求关闭通道。该拆分和 typed-request authorization 已有非破坏性 CTest 覆盖；定长 grant/WorkerReady 与 typed request 经隔离子进程往返验证。`portable_helper_authorization` 和 `protocol` 2/2 PASS，授权 CTest 连续 10 次通过。使用隔离 `%TEMP%` 的 `chast-host-parity-test --elevation` 全套回归通过；首次默认 TEMP 运行因用户 LocalAppData Temp 访问拒绝未能创建测试目录，随后隔离到 `build/test-temp` 重跑通过。没有触发真实 UAC、创建服务或写 ProgramData/防火墙。该模块仍**未连接**实际 SCM worker、远程会话权限或桌面捕获/输入注入；`BeginGrant` 没有 UAC 授权调用方，因此不能宣称 H01 授权功能已接通或可用于生产。真实 SCM/桌面操作均未触发。

2026-11-26 有界撤权监督核心：新增 `host/portable_helper_supervisor.{h,cc}`，把取消、有界 lease drain 与 worker 正常退出等待固化为可注入顺序；任一有界等待超时才强制终止，确认进程退出并再次 drain 后才释放输入；若终止/退出/二次 drain 失败，会关闭通道但不释放仍可能被按住的输入，授权保持 Revoking，可在 lease 后续排空后重试。Release 构建后运行 `ctest --test-dir build/root -C Release -R '^portable_helper_authorization$' --repeat until-fail:10 --output-on-failure`，**10/10 PASS**；回归验证协作退出、无操作但 worker 不退出时强制结束、在途 lease 超时、终止失败时不提前释放输入及后续重试。Win32 回调现已在隔离同用户子进程上真实绑定进程句柄和已认证 Named Pipe 并完成 Stop/退出等待；但尚未从真实 SCM `SERVICE_STATUS_PROCESS` 获取 SYSTEM helper PID/身份，未接入正式服务 pipe 循环或真实桌面注入回调；因此仍未形成产品 worker 闭环。验证未安装服务、触发 UAC、写 ProgramData/防火墙或注入真实输入；Release 链接仍显示既有 `LNK4098`。

## 目标与当前差距

当前主机并非缺少提权入口：`host/elevation_handoff.cc` 使用本机 UAC 启动同映像的提权子进程，通过随机命名管道校验 PID、进程创建时间、会话和映像，传递原用户身份句柄及内存配置；提权主机报告同一设备指纹并监听就绪后，旧主机才退出。失败时旧主机恢复监听。这是一次性进程接管，仍会断开所有现有会话。

当前已安装的 `chast-host` SCM 服务运行于 SYSTEM，但服务通过 `WTSQueryUserToken` 启动普通交互用户的 `chast-host` 管理器和 worker。它使用固定服务名 `chast-host`、机器设置和正式服务生命周期，不是可按需创建、可重复承接桌面切换的便携辅助服务。

H01 要补的是**会话期间可复用、由本机用户明确授权的特权桌面辅助能力**。普通查看会话仍由交互用户主机持有网络连接、设备身份、认证和远程权限；辅助进程只提供为当前授权会话所需的桌面能力，不成为第二个网络主机或新的认证主体。

## 安全边界

1. 只有已认证的桌面会话、持有有效键鼠控制权且未被撤权时，远端才能请求本机提示本地用户授权。远端请求不得静默创建服务或绕过本机 UAC。
2. Windows 管理员凭据不得通过远控协议、命令行、普通配置文件、日志或崩溃转储传递。每次新授权均由本机用户在系统 UAC 界面完成。
3. 辅助进程绑定到唯一的 Windows logon SID、Windows session、父进程创建时间、主机实例身份和会话代次。重连、换账号、换设备身份或授权撤销后，旧 capability 立即失效。
4. IPC 使用拒绝远程客户端的单实例命名管道；ACL 仅授予 SYSTEM、管理员和本次授权的 logon SID。连接后再次校验客户端 SID、PID、会话、映像和进程创建时间。随机端点值只作定位，不代替身份认证。
5. IPC 仅提供具名桌面操作和有界数据帧。禁止通用命令执行、任意进程启动、任意文件/注册表访问、通用句柄复制和跨会话转发。每条输入仍由正常主机按实时会话权限检查。
6. 辅助服务只在显式批准后运行，不新增开机启动项，不改变正式 `chast-host` 服务配置、防火墙或机器策略。拒绝、超时、断线、用户注销、父进程异常退出和辅助进程崩溃均撤销本次授权并停止辅助工作进程。
7. 辅助二进制必须从受保护的暂存目录运行；服务名、管道端点和暂存目录均使用本次启动的随机实例 ID。清理只允许删除本实例创建且映像路径与实例记录匹配的服务和文件，不能按通配符清理或触碰已安装服务。

## 架构决策与实现顺序

保持现有 `ElevationHandoff` 作为“将整个主机切换到管理员令牌”的独立能力，不把它改造成后台 broker。新辅助服务使用独立的服务入口和版本化 IPC，避免和固定服务名 `chast-host` 的安装、升级、卸载流程互相修改。提权服务负责生命周期、SYSTEM 身份及用户会话绑定；会话 worker 只在被授权的交互会话中提供桌面捕获/输入适配。网络连接和设备密钥仍留在原主机，辅助 worker 不接入注册或中继。

实现分为四个垂直切片：

1. **生命周期外壳**：随机服务实例标识、显式创建/启动/停止/删除状态机、父进程与 logon SID 绑定、幂等清理及进程崩溃恢复。服务安装失败时不启动 worker；删除失败必须留下可识别的待清理状态，不吞掉错误。
2. **认证 IPC**：加入具名消息版本、随机挑战、实例 ID、能力票据和连接身份复核；拒绝重放、跨会话客户端、过期代次、超限消息和未授权操作。凭据、访问令牌、TOTP 密钥和主机设置不放进命令行或普通持久化文件。
3. **桌面能力桥接**：将当前捕获/输入桌面边界接入 helper worker，仅转发获准的桌面帧与输入操作；会话权限撤销时先停止输入并清除按键状态，再关闭帧通道。主机 UI 明确显示辅助服务状态和本地授权状态。
4. **升级与退出**：新版本先在受保护目录暂存并校验，旧 worker 仍可服务；新 worker 完成身份确认和桌面能力探测后才切换。切换失败恢复旧 worker。正常退出、崩溃和系统重启均删除临时 SCM 项并停止对应进程；不能删除其他实例或正式服务。

先完成第 1、2 切片及其隔离测试，再接入桌面能力；不得把“成功创建临时服务”报告为 UAC/安全桌面功能完成。

## 验收门槛

- 自动化：服务生命周期的成功、重复启动、拒绝、超时、父进程强杀、helper 强杀、服务删除失败；IPC 的错误 SID/PID/session/image、重放挑战、旧 generation、权限撤销、畸形/超限消息；主机网络身份和现有服务配置保持不变。
- 单机 Windows：标准用户批准/拒绝 UAC，已提权管理员，UAC 安全桌面进入/退出，锁屏/解锁，RDP 与控制台会话变化，多会话并发，管理员注销，辅助进程异常退出，主机更新与回滚。
- 每项记录辅助进程令牌身份、session/logon SID、服务实例 ID、设备指纹、连接代次、服务/进程退出码和桌面恢复结果；凭据和访问令牌不得进入日志。
- 发布门槛：无残留临时服务/worker/暂存文件；撤权后不能注入输入或继续采集；原有连接在支持范围内保持，超出能力时明确断开并可按原指纹重连；真实安全桌面和双机证据单独记录。

## 依赖与限制

- 当前固定名服务及现有服务 worker 不能直接复用为随机临时服务实例；需要独立入口或独立 helper 二进制，并确认服务控制、日志、安装包和更新清单都不会误操作正式服务。
- 真实安全桌面结果取决于 Windows 版本、会话类型及服务/worker 桌面 ACL。自动化 mock、默认桌面测试或一次普通 UAC 重启都不能证明安全桌面覆盖。
- 当前已新增生命周期协调器和 Win32 SCM adapter 代码，但未调用 SCM API、创建 staging 文件、写入 ProgramData/防火墙或触发本机 UAC。实现仍以 fake backend 和代码路径校验为主；独立 helper service entry、受保护 staging/copy 与恢复记录完成后，再在隔离机器上验收真实服务。

## 现有提权链路基线

2026-11-25 在当前分支重建并运行 `build/Release/chast-host-parity-test.exe --elevation`，专项通过。覆盖远端权限/控制权准入、同一时间仅一个 UAC 请求、撤权/断线取消、实际同映像子进程、私有管道父子身份校验、原用户 DPAPI/TOTP 认证材料继续可用、身份不匹配时恢复旧监听、提交前取消不抢占端口。

该回归使用可注入的 UAC launcher，不弹出真实 UAC；测试主机使用隔离 loopback，不能证明 SYSTEM helper、服务安装/清理、UAC 安全桌面、锁屏或跨管理员账户行为。它是本次 H01 新实现前的基线，不替代新增服务生命周期和 IPC 的专门回归。

## IPC 实现切片：帧协议和本机 Named Pipe

`host/portable_helper_protocol.{h,cc}` 增加独立于现有一次性 handoff 的版本化本地帧格式：固定 magic/header、16 字节随机 instance ID、消息类型、单向 sequence、request ID、payload 长度和保留 flags。payload 上限为 1 MiB；解码严格拒绝未知版本/类型/flags、错误长度、尾随数据、截断帧及空 instance ID。`PortableHelperSequence` 对每个方向独立检查 instance ID 和从 1 开始的连续序号，拒绝重复、跳号或跨 helper 实例的消息。

`PortableHelperPipe` 创建拒绝远程客户端、仅 SYSTEM/管理员/显式授权 logon SID 可访问的单实例 message-mode 命名管道。创建者必须从已授权的交互用户 token（例如 SYSTEM 服务用 `WTSQueryUserToken` 取得的 token）导出 logon SID；不能从 SYSTEM 服务自己的 token 推断授权用户。客户端在打开后核对服务端 PID、Windows session、映像路径、进程创建时间、用户 SID 和 logon SID；服务端在接受后对客户端执行同样核对。收发使用 overlapped I/O 和调用方时限，不将管理员凭据放入端点或命令行；端点由随机 instance ID 派生。peer 身份核对是双向的，但它仍不等于 Authenticode 发布者验证，也不允许仅凭同属 SYSTEM 就接受未知映像；服务端调用者必须将 SCM 启动记录中的确切 worker 进程身份作为 expected peer 传入。

帧的 type、request ID、instance ID 和连续序号只提供协议边界与防重放序号，不单独代表一次本地授权。2026-11-26 新增 `CaptureFrameRequest` / `InputEventRequest` 严格类型消息：载荷固定包含连接 ID、generation、Windows session；输入体复用现有 24 字节 InputEvent 编码和解析器，按 Key/LegacyKey 与 Mouse/Touch/Pen/Gesture 映射到独立键盘/指针 capability。授权模块只接收这两种桌面操作类型，拒绝通用命令、错 scope、错操作及畸形/多余载荷。每个获准请求返回 RAII operation lease；撤权先关闭新请求、调用取消回调并等待在途副作用结束，随后清除输入；监督者可使用限时 drain API 决定是否终止卡住的 worker，drain 后再调用输入释放。清理失败仍要求关闭 IPC。该消息级授权核心已有非破坏性自动化覆盖，但尚无正式 SCM worker 调用方，不能把它视为桌面能力产品闭环；真实 worker 仍需接入有界取消/强制终止策略。

Release `chast-host-parity-test --elevation` 已重建并通过新增帧往返、版本/类型/flags/长度错误、超限 payload、重放、跳号和跨实例回归；还通过隔离子进程实际打开受 ACL 限制的 Named Pipe，核对双方 PID/session/映像/创建时间/用户 SID/logon SID，并完成双向帧收发。补充的传输失败回归确认：本地拒绝超限发送不消耗序号；accept/receive 超时会取消 I/O 并关闭端点；对端断开或预期 logon SID 不匹配后通道不可复用；非法授权 SID 会被拒绝。管道 ACL 的 logon SID 现由调用方显式传入，避免 SYSTEM 服务错误地使用自身服务 token 的 logon SID。该验收只运行同用户测试子进程；它没有创建 SCM 服务、启动 SYSTEM helper worker 或验证 UAC 安全桌面。既有 UAC handoff 全套用例也通过。下一切片需接入唯一临时服务实例、将 SCM 启动记录中的确切 worker 身份注入管道两端，再按会话撤权/父进程断开停止该 worker。

## 生命周期协调器实现切片

`host/portable_helper_lifecycle.{h,cc}` 加入可注入后端的单实例状态协调器和 Win32 SCM adapter。服务名由安全随机生成的非零 128-bit instance ID 派生为 `ChastPortableHelper-<hex-id>`，与固定名 `chast-host` 分离；服务规格要求固定的每实例 ProgramData 暂存路径及带进程创建时间、用户 SID、logon SID 和交互 session 的父进程身份。Win32 adapter 只创建 demand-start、own-process、LocalSystem 服务；启动前核验暂存映像 ACL、当前父进程身份、服务映像路径/类型/启动方式/账户及服务控制 DACL；停止和删除也先核验实例名和配置。`StartService` 参数只带 instance ID 与父进程身份，不带凭据。协调器状态 `Running` 只表示 SCM 报告 SERVICE_RUNNING，不能替代经过 IPC 验证的 worker-ready 状态。

状态覆盖 NotCreated、Created、Running、CleanupPending、Removed。启动失败会尝试停止并删除本实例；停止失败时不删除仍可能运行的服务；删除失败保留 CleanupPending，后续可重试；重复 start/cleanup 不创建或删除其他实例。新增 `chast-portable-helper-lifecycle-test` fake-backend 覆盖正常启停、名字冲突、非法服务名/映像路径、启动失败回滚、停止失败阻止删除及删除失败重试。独立 SCM service entry 已在后续切片实现，但受保护 staging/copy 工具和目标暂存文件仍不存在；本轮没有调用 Win32 adapter，因此 fake 测试不证明真实 SCM 可创建、运行或清理。父进程崩溃后的跨进程恢复记录、SYSTEM worker、桌面桥接和 UAC/安全桌面仍待实现/验收。下一步实现受保护 staging/copy 和恢复记录，再在隔离环境实际验收 Win32 SCM 生命周期；在 worker readiness 经 IPC 确认前不得开放 capability。

## 独立 SCM 服务入口实现切片

2026-11-25 新增 `host/portable_helper_service.{h,cc}`，并在 `chast-host.exe` 中加入提前分流的 `--service-name <instance-derived-name>` 服务入口。该入口直接进入 `StartServiceCtrlDispatcherW`，不创建托盘窗口或普通主机服务；SCM `StartService` 参数必须恰好包含实例 ID、父进程 PID/创建时间/session/image/user SID/logon SID。解析器拒绝缺项、额外参数、非十进制数字、溢出、非十六进制实例 ID 和服务名/实例不匹配。

服务入口启动后核对自身为 Session 0 的 LocalSystem 进程、自身映像等于本实例 ProgramData 路径且文件 ACL 受信、父进程身份与创建时间完全匹配；随后等待 SCM stop/shutdown 或该父进程退出。没有桌面、网络、设置读取或 IPC worker 初始化。`SERVICE_RUNNING` 仅说明参数和进程身份通过检查、父进程仍存活；不能作为 helper 已认证就绪的信号。

Release `chast-host`、`chast-portable-helper-lifecycle-test` 和 `chast-host-parity-test` 构建通过；`portable_helper_lifecycle` CTest 1/1 通过，覆盖精确参数解析及损坏参数/跨实例拒绝；`chast-host-parity-test --elevation` 全套既有提权回归也通过。普通进程直接启动该服务入口时，`StartServiceCtrlDispatcherW` 按预期返回“未由 SCM 启动”，探针退出码为 2；没有安装 SCM 服务或写入 ProgramData。这个入口尚未通过真实 SCM 启停回归，且暂存映像尚不存在。

## 受保护暂存与复制实现切片

2026-11-25 新增 `host/portable_helper_staging.{h,cc}`。`stage_portable_helper_service_image` 自行生成密码学随机的非零实例 ID，要求当前进程具有本地 elevated administrator token 和交互 session，源映像固定取当前进程身份中的 image path，不接受外部任意路径；它按实例 ID 派生 ProgramData 目标，已存在的 `Chast`/`PortableHelper` 目录必须先通过机器状态 ACL 信任检查，新建目录使用受保护 DACL 并逐层硬化。每个实例目录独占创建，遇到同名目录立即失败，不触碰已有实例。

复制器通过禁止写入/删除共享的 no-follow 句柄打开源映像，拒绝目录、reparse point、多硬链接、非 PE 文件、截断 header 和超过 512 MiB 的映像；目标以 `CREATE_NEW` 创建，逐字节流式复制并 Flush，完成后再硬化/核验目标文件 ACL。复制或核验失败时仅删除本次创建的目标文件及本实例空目录；共享根目录不会递归清理。该 API 尚未接入本地授权/UAC 编排，因此本轮没有调用它，也没有创建 ProgramData 文件或目录。

当前复制器把“当前进程的映像路径”作为源选择边界，但尚未校验 Authenticode 发布者或签名哈希；目标 ACL 校验只证明落盘文件不可被普通用户替换，不证明该文件来自预期发布者。接入 UAC 流程前须明确签名/发布者固定策略；本 API 不能单独作为生产 SYSTEM 服务的映像真实性验收。

2026-11-25 补充发布者策略：CMake 配置项 `CHAST_PORTABLE_HELPER_PUBLISHER_CERT_SHA256` 固定发布者签名证书的 SHA-256 thumbprint，必须恰好 64 位十六进制；未配置时 elevated staging 和 Win32 SCM adapter 均失败关闭。暂存前在禁止源文件写入/删除共享的句柄仍持有期间调用 WinVerifyTrust，要求整条签名链可信并执行整链吊销检查，然后精确比较叶签名证书 SHA-256；复制并硬化目标后再验证一次。当前工作区的 `build/Release/chast-host.exe` 是未签名文件，`build/CMakeCache.txt` 的 pin 为空，因此这台构建不能创建 helper 服务。发布流水线必须用实际代码签名证书指纹重新配置并签名；离线且系统未缓存撤销材料时会按失败关闭处理。尚未获得项目发布证书，故没有填入或臆造 pin，也没有正向有效发布者链路的真实签名样本。

Release 生命周期 CTest 的隔离临时目录回归验证当前测试映像逐字节复制、已有目标不被覆盖、非法 PE 在创建目标前拒绝及硬链接源拒绝；现有生命周期用例另覆盖随机实例 ID 非零。`chast-host`、生命周期测试和提权回归构建通过，`portable_helper_lifecycle` 1/1 PASS，既有 `chast-host-parity-test --elevation` 通过。此测试不验证管理员 token、真实 ProgramData ACL、UAC 时序或实际 SCM 生命周期。

2026-11-25 随后将 staging 清理接入生命周期：正常清理顺序现为停止服务、删除本实例 SCM 项、确认 SCM 已不再能打开该服务，然后通过受保护 ACL/no-follow 文件句柄删除单链接映像并移除空实例目录。若 SCM 项仍运行、仍存在或处于 marked-for-delete 状态，暂存映像会保留；服务删除或映像删除任一步失败都会显示 `CleanupPending`，重试时重新确认 SCM 与文件身份。启动/创建失败后的暂存回滚也遵循同样的服务不存在检查，遇到同名服务冲突时绝不删掉它的映像路径。fake backend 新增服务删除后映像清理、映像清理失败/重试和服务名冲突保留映像覆盖，Release `portable_helper_lifecycle` CTest 1/1 PASS。Win32 SCM/ProgramData 清理路径尚未在真实管理员环境运行。

## 跨进程恢复记录实现切片

2026-11-25 新增 `host/portable_helper_recovery.{h,cc}`，在实例目录中以 `recovery.dat` 保存版本化、定长头加有界 UTF-16 字段的非秘密记录，内容绑定随机实例 ID、派生服务名/映像路径及父进程 PID、创建时间、session、映像、用户 SID 和 logon SID。记录写入采用独占临时文件、Flush、硬化文件 DACL 后原子改名；读取核对受保护目录/文件、无 reparse、单硬链接、大小、格式、版本和规范实例规格。服务入口必须让 SCM 参数与恢复记录一致后才继续；服务和映像清理也先读取并核对该记录，清理顺序保留记录至映像删除完成。

隔离回归覆盖记录往返、错误 magic、截断、尾随数据和非规范规格拒绝，合并生命周期/复制测试 `portable_helper_lifecycle` CTest 1/1 PASS；Release `chast-host` 构建及既有 `chast-host-parity-test --elevation` 通过。记录受机器状态 ACL 保护，不保存凭据/访问令牌；记录文件不是 UAC 批准凭证，仍由后续本地批准流程控制创建服务。

## 正式服务启动恢复扫描接线

2026-11-25 将 `scan_and_recover_portable_helper_instances` 接入正式 `chast-service`：服务报告 `SERVICE_RUNNING` 后启动独立后台线程，扫描受保护的 ProgramData 实例根目录。恢复失败不会中止正式服务；日志记录扫描失败，或 inspected/active/cleaned/pending/invalid/interrupted 汇总。停止/关机信号会阻止扫描线程开始处理后续记录；若正在执行 SCM 停止等单实例操作，则完成当前操作后退出。正式服务在退出前等待线程结束并继续更新 STOP_PENDING 状态，不在产品服务主循环中同步执行全量恢复。

恢复 API 增加可注入的继续检查；fake backend 回归确认可在首个操作前取消，也可在停止服务后暂停当前实例的后续删除，供下一轮扫描安全重试。Release `chast-service`、`chast-portable-helper-lifecycle-test` 构建通过，`portable_helper_lifecycle` CTest 1/1 PASS，`git diff --check` 通过；`chast-service --run` 在 SCM 外按预期拒绝运行。尚未在已安装的真实 SCM 服务中观察扫描日志，也未在真实 ProgramData/服务目录创建或清理实例。本地验证证明的是构建和注入式生命周期语义，不证明机器启动后实际服务执行成功。

## 当前未完成项与顺序

1. **配置发布签名并接通本地 UAC 授权**：发布者 pin 机制已定为 CMake `CHAST_PORTABLE_HELPER_PUBLISHER_CERT_SHA256`，缺 pin 或签名/信任链/吊销验证失败时拒绝 staging 和 SCM 启动；尚需取得项目实际 Authenticode 发布证书、配置其 SHA-256，并用签名产物验证正向链路。随后将 staging API 和生命周期接入本地授权流程，验证拒绝/取消不落盘，并在隔离管理员环境核验 ProgramData ACL、服务删除及映像清理重试。
2. **恢复扫描实机验收与安全重试**：后台扫描已接入正式服务，继续覆盖父进程强杀、helper 强杀、服务删除失败、SCM marked-for-delete、系统重启及损坏/未知文件隔离；在隔离管理员环境观察真实 SCM/ProgramData 扫描日志及失败重试，确认停止过程、正式服务可用性和残留状态。
3. **真实 SCM 验收**：在隔离 Windows 环境由明确授权的管理员路径真实安装、启动、停止、删除临时服务，核对服务进程 PID/映像/令牌身份，验证失败回滚且正式 `chast-host` 配置不变。fake backend 与直接运行 dispatcher 探针都不能替代此项。
4. **授权 IPC 与 worker-ready**：本地 capability 状态机、HMAC 挑战 proof、worker PID 身份比对、目标 session 绑定、代次与逐操作 gate、严格类型化的 capture/input 请求、在途 operation lease 及 split Begin/Wait/Finish revocation 已有实现；定长 grant/WorkerReady 交换已接入 `PortableHelperPipe` 并由隔离子进程测试覆盖。下一步须将它接入真实 SCM worker，父进程从 SCM 取得并校验确切 worker 身份，并要求真实本地 UAC 批准后才可调用 `BeginGrant`；再由真实 worker 在每条桌面捕获/输入副作用前 acquire lease。退出、撤权、会话切换须封闭新操作，接入取消和超时后的 worker 强制终止，再清除按键状态、排空 lease 后关闭帧通道；只有全部在线校验完成后才向主机确认 WorkerReady。当前无正式调用方，未形成产品闭环。
5. **桌面能力与产品交互**：接入限定的桌面捕获/输入 worker、本地授权状态 UI、UAC 拒绝/超时/撤权、锁屏/安全桌面及 session 切换恢复。远程网络身份、设备密钥和认证仍由原主机持有。
6. **升级退出与发布验收**：实现新旧 worker 切换/失败回滚、父子进程与系统重启清理，再完成管理员/标准用户、RDP/控制台、多会话、锁屏、安全桌面等实机矩阵；发布前确认无临时服务、进程或暂存文件残留。
