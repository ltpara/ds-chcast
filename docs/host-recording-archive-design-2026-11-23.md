# H03 主机入站录制归档设计

日期：2026-11-23

## 范围

本设计只覆盖 Windows 主机端已经封口的入站录制分段上传到账号归档；不替代查看端已有的录制上传队列，也不改变主机本地录制默认行为。H03 完成仍需主机采集、可恢复上传、服务端账号归属和归档保留闭环。

## 进程边界

- C++ `chast-host` 负责在分段成功封口后写入本地持久任务；任务只保存源文件路径、文件身份/大小、分段时间信息和稳定幂等键，不保存 bearer、注册密钥或设备私钥。
- Go `chast-tunnel host` 是上传执行方：它已持有设备 Ed25519 私钥、主机 API 凭据、HTTPS CA/代理配置，可直接为请求签名并发送分片。默认关闭时不得启动 worker、扫描任务目录或发送请求。
- 录制任务不复用现有 audit JSONL spool。audit spool 的语义是事件被服务端接受后删除，而录制任务必须跨重启保留文件/任务，并以服务端 offset 恢复。
- 多个 agent 进程需通过操作系统级 worker lease 互斥；任务创建与状态写入采用原子替换，不能依赖单进程 `std::mutex`。

## 安全与归属

- 云端上传必须显式启用，默认关闭；关闭时主机只保留本地录制，不扫描或发送归档任务。
- 只允许 HTTPS 服务端接收远程录制数据；服务端路由拒绝非 loopback 的明文 HTTP，`localhost`/loopback 是隔离验收例外。主机 agent 后续仍须配置 HTTPS URL，并沿用现有 TLS CA、代理和服务端身份固定配置。
- 每个上传请求同时验证主机 API 凭据和该设备已注册 Ed25519 私钥签名。账号及用户名不得由主机请求体指定。
- 归档账号只从服务端当前设备注册记录的 `owner` 解析。owner 为空、账号不存在或账号已封禁时拒绝上传，并保留本机文件和持久任务。
- 上传会话固定在创建时解析出的账号。账号库持久化 `(host_id, idempotency_key) -> user_id` 不可变绑定，owner 后续变化时不得转移或重归属；旧会话不再接受新请求，按现有闲置上传会话回收策略释放服务端配额。本机源文件继续保留，直到该任务明确成功或由本机操作者处理。
- 每个封口分段使用绑定设备、服务器、会话/分段身份的稳定幂等键。相同任务重试必须返回已有会话/归档，不产生重复录制。

## 恢复、限额与文件保留

- 主机录制器只在 MP4 trailer 写入并关闭成功后发布完成任务；不得通过扫描或猜测正在写入的 MP4 来判定封口。
- 本机任务队列原子持久化服务器身份、设备 ID、源文件路径、分段元数据、幂等键和服务端 upload ID；不保存账号 bearer token。恢复时以服务端 offset 为准续传。
- 只有完整、可读且非空的 MP4 分段进入上传队列。崩溃导致的未封口文件保留并标记为待人工处理，不自动上传或删除。
- 服务端继续使用账号归档现有文件大小上限、活动上传并发上限、账号配额、SHA-256 校验和保留清理。额度或保留策略拒绝/清理归档时，不改变设备本机源文件。
- 云端归档成功后也不自动删除主机源文件；本地清理属于独立的本机保留策略，待单独定义并验收。

## 验收边界

先完成设备签名 API 与 owner 隔离测试，再完成封口事件、本机持久队列/锁、分片续传和幂等恢复。验收至少覆盖默认关闭、无 owner、封禁 owner、owner 中途变更、服务端中断/重启、重复完成、配额拒绝、损坏/未封口 MP4，以及本机文件不被意外删除。所有过程使用隔离目录与 loopback 服务；不安装正式 Windows 服务，不修改真实 ProgramData 或防火墙。双机及生产部署仍按总计划后置。

## 当前进度（2026-11-23）

服务端设备签名上传接口已实现：主机 API 凭据与注册设备 Ed25519 签名双重校验；账号 owner 从注册表读取；每个分片的 SHA-256 纳入设备签名并在写入时校验，摘要不符会回滚到旧 offset。非 loopback 明文 HTTP 被拒绝；账号库持久化设备任务键与首次 owner 的绑定，重复 begin 不会把录制另建到新 owner 账号。接口复用现有归档会话、账号配额、保留清理、幂等键和完成审计。定向与相关 Go 包完整测试覆盖签名域分离、分片摘要失败回滚、无 owner 拒绝、owner 归属、幂等开始、完成归档、owner 变化后拒绝续传及绑定并发竞争。

2026-11-23 后续推进：`StreamRecorder` 现提供分段封口回调；只有 MP4 trailer 写入、文件关闭均成功后才通知消费者，并携带已封口文件路径。复用器公开已封口分段列表，格式内部轮转后会及时发出事件，不必等待后续分段结束。I/O 失败不触发对应文件通知；模拟持久队列回调抛错时，已封口本地 MP4 仍可重开且不会删除。Windows Release `mp4_recorder` 回归通过，覆盖轮转期间通知、重连、失败保留及输出目录 I/O 错误。

2026-11-23 Go agent 推进：`chast-tunnel host` 新增默认关闭的 `--upload-recordings` 和 `--recording-queue`；仅 opt-in 时启动任务扫描。worker 读取 `<32-hex-id>.json` 不可变任务，校验 MP4 路径/大小，使用主机凭据和设备 Ed25519 对 begin/chunk/complete 签名；每次重试重新幂等 begin，以服务端 offset 续传。分片 SHA-256 纳入签名；只有完成归档后删除任务清单，本地 MP4 保留。Windows 使用命名 mutex、Unix 使用文件锁协调多个 worker。agent 定向回归和 `go vet` 通过，测试模拟服务端已接收分片但响应丢失，再次运行从服务端 offset 续传，并以子进程验证 worker 锁。

2026-11-23 C++ 主机接线完成：`upload_recordings` 默认为关闭；显式启用时，从绝对设置文件路径派生持久队列目录，并仅向 `chast-tunnel` 传入 `--upload-recordings`/`--recording-queue`。`Mp4Recorder` 只把 trailer 写入且文件关闭成功的分段标为完成；`StreamRecorder` 在分段封口时回调路径、开始时间和时长。主机以文件身份和分段元数据生成稳定任务 ID，校验 MP4 源文件，并通过 write-through 临时文件加原子改名发布清单；相同清单幂等成功，冲突失败。队列写入失败只记录错误，不删除本地 MP4。Release `host_settings`、`host_tunnel`、`mp4_recorder`、`recording_upload_queue` CTest 4/4 通过；`go test ./internal/agent ./cmd/chast-tunnel -count=1` 与 `go vet ./internal/agent ./cmd/chast-tunnel` 通过。`tools/test-host-recording-archive-e2e.ps1` 已通过：C++ producer 生成任务，Go worker 通过隔离 loopback fixture 完成签名分片上传，清除清单并保留源 MP4。

2026-11-23 编译态 tunnel 进程验收完成：上述 PowerShell E2E 入口另行构建并启动真实 `chast-tunnel.exe host`，使用测试私有 CA 和 HTTP CONNECT 代理。隔离服务端已持久接收首个 4 MiB 分片后，fixture 丢弃成功响应，强杀首个进程；第二个 tunnel 进程重新启动并从服务端 4 MiB offset 继续，最终校验归档文件逐字节和 SHA-256、任务清单已删除且源 MP4 保留。该父子进程测试曾因夹具跨越 Windows Mandatory Integrity 边界而误报删除失败：Release C++ harness 带 Low Integrity 标签，Go 测试进程却在较高完整性目录生成清单。现改由同一 C++ producer 在隔离构建目录生成父子进程用的大文件任务，匹配实际 producer/consumer 完整性；未放宽 ACL、未改变进程 token。修正后的完整 PowerShell E2E 连续两次通过；相关 Release CTest 4/4、`go test ./internal/agent ./cmd/chast-tunnel -count=1` 和 `go vet ./internal/agent ./cmd/chast-tunnel` 均通过。

2026-11-23 真实主机整链 loopback 验收完成：测试构建的便携 `chast-host` 在隔离配置中加载 `record_incoming`/`upload_recordings`，由仅合成 BGRA 图样的捕获器和真实软件 VP9 编码器产生视频；固定证书指纹的独立 Desktop probe 收到五帧后正常结束会话。录制器实际封口 MP4，`host/tunnel.cc` 启动真实 `chast-tunnel` 子进程上传；测试核对注册 owner 下恰有一份归档、归档大小与 SHA-256、源文件字节一致且源 MP4 仍保留。完整 `tools/test-host-recording-archive-e2e.ps1` 全部通过，包含 C++ queue producer、tunnel 独立恢复、`TunnelProcess` 父子进程强杀恢复及真实主机归档；主机验收单项用时约 2.4 秒、MP4 2278 字节。夹具仅在测试构建、`CHAST_NATIVE_TESTING=1`、专用 fixture 变量、便携独立 loopback 配置及配置/录制/队列路径全部位于本次 `build/test-temp` 隔离根目录内时启用；只读取已有用户证书，缺少证书则跳过，不安装服务、不写真实 ProgramData/防火墙、不捕获桌面内容。

H03 本机代码链与单机 loopback 整链验收现已完成。正式 Windows 服务安装、真实 ProgramData/防火墙操作、生产部署及双机验收仍按既定边界后置。主机源 MP4 上传成功后仍永久保留，自动清理策略尚未定义。
