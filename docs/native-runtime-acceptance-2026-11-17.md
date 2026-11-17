# W1 / S01–S02 Windows 原生进程联动验收

日期：2026-11-17。分支 `codex/s03-role-console-2026-11-17`，基线 `202a228` 加当前工作区。未提交、推送或切换分支。

## 1. 结论与范围

本轮完成本机真实 Windows 托盘主机、隧道代理和独立 QUIC 查看端的联动验收，补上 [上一轮浏览器模拟回执](strategy-connection-console-acceptance-2026-11-17.md) 未覆盖的实际进程执行证据。精确断开、另一连接保持、控制面中断、回执上传失败后代理恢复、带连接的主机异常退出、新 runtime 隔离、策略应用及撤回均通过。

没有发现需要修改生产逻辑的新缺陷。本轮新增的是测试工具、显式启用的集成用例、构建目标和执行脚本；不修改协议或 schema（仍为 v26）。

验收边界：

- `chast-host.exe` 为实际托盘程序，以隔离 `--portable --tray --config` 启动，实际消费策略、发布连接快照并执行断开；没有使用 FakeHost。
- `chast-tunnel.exe` 为实际 Go 程序，初始由托盘启动，故障后另行启动同一程序，签名上传生产协议快照/回执并持久化命令。
- 每个查看端为独立 `chast-live-viewer-probe.exe` 进程，使用生产 `QuicSession`、固定证书指纹和实际握手。连接是文件模式，不采集屏幕、不传文件、不注入输入；没有启动 Flutter UI。
- 管理 API 通过 `testutil` 启动真实处理器、SQLite、身份/注册与中继组件；HTTP 故障由仅回环的测试代理注入。未启动独立 `chast-server.exe` 或生产数据库。
- 所有程序运行在同一台 Windows，网络为 loopback；未安装 Windows 服务、修改已有主机配置或防火墙。测试设备/密钥均为虚构数据，LAN 发现关闭。

这是一项真实 Windows 进程集成验收，不能扩大为双物理机、跨 NAT、SCM 服务、桌面流或完整发布验收。

## 2. 实际执行结果

| 场景 | 操作与判据 | 结果 |
| --- | --- | --- |
| 实际主机注册 | 启动托盘，等待其实际 tunnel 注册设备与指纹，并产生原生快照 | 通过 |
| 多查看端 | 分别启动 A/B 两个进程，完成固定指纹的 QUIC 握手，API 收到两条不同连接 | 通过 |
| 精确断开 | HTTP 创建 A 的指令，等待 A 的实际传输断开、原生连接消失及服务端接受 applied + confirmed_absent | 通过；B 的传输和 API 活动态保留 |
| 同一次请求重试 | A 结束后以相同 request key 再发请求 | 返回同一个 command ID |
| 控制面暂时不可用 | 代理返回 503，再创建 B 的断开请求；保持 6 秒并确认真实 B 仍连接 | 指令保持 queued，不误报成功 |
| 原生已执行但回执上传失败 | 恢复其他 API，只阻断 receipts；B 实际断开，原生文件留下明确回执 | 服务端保持 delivered、未确认；连接消失不替代签名回执 |
| agent 异常退出并恢复 | 只终止该托盘的 tunnel 子进程，重启同一实际二进制并复用原文件 | 恢复上传已落盘回执，原 command ID 变为 applied + confirmed_absent |
| 带活动连接的主机异常退出 | 再连一个查看端，强制终止测试托盘；查看端从传输检测断开 | 通过；探针 idle timeout 为 15 秒，生产查看端超时未改 |
| 主机重启 | 用同一个隔离配置启动实际托盘，重新连接查看端 | runtime ID 更新，新连接正常到达 API |
| 非空旧 runtime 指令 | 暂停控制面，注入旧 runtime 的非空命令文件，并特意指向当前新连接、使用未来截止时间 | 新连接仍在，原生没有产生该命令回执；不是空队列测试 |
| 策略应用 | 向 nativebox 分配 `host.max_viewers=3` locked 策略 | 实际监听器重启，runtime 更新，当前查看端结束；服务端收到对应当前 revision/digest 的真实 applied 回执 |
| 策略撤回 | 删除分配，等待新的当前版本回执 | applied；空覆盖恢复，本地配置仍为 `max_viewers=2`，未把受管值写成本地偏好 |

策略测试验证实际应用/监听重启/回执链路；本轮没有验证受管票据下同时接入三个查看端，因此不把“最大连接数恰为 3”的准入边界记为已测。启动项变更会结束现有连接，已按当前实现记录其影响。

旧 runtime 场景是主动构造本地缓存反例：保留旧 runtime、使用新连接 ID 和未来 deadline，使运行实例校验成为必要条件。它不代表通过了远程攻击者伪造服务器签名；签名与授权校验另有现有 API/agent 回归。

## 3. 运行证据

环境：Windows `10.0.26200.0`、Go `go1.27.4 windows/amd64`、CMake Release 原生构建。完整最终执行目录：

`build/native-runtime-20261117-143043-17998e38/`

- `build.log`：重新构建 `chast-host`、其依赖 `chast-tunnel` 和新查看端探针成功。保留既有运行库冲突及未使用延迟导入链接警告。
- `acceptance.log`：`TestWindowsNativeRuntimeAcceptance` **通过，115.24 秒**；Go 包含初始化共 120.180 秒。该结果是一项跨进程综合用例，不是 12 个独立测试。
- `manifest.json`：分支、基线、系统/Go 版本及三个二进制、测试源码和脚本的 SHA-256。
- `run-4279171062/`：查看端连接/断开标记及日志、恢复代理日志、原生当前快照/命令文件、策略缓存/回执，以及注入的 `replayed-old-runtime.json`。

可追溯目标：首个 runtime 为 `cef52d11183c5bfb58cc425879a51c4c`，被断开连接为 `c2730402fa6ce793fc6a76ce4d8bdc4f`，指令为 `5f5151f0cdd87a043cdff97316a88959`；同时保持的连接为 `4e2dd2ce0bdf98024c57fa0826e9d37a`。进程重启后的 runtime 为 `f503d4910a48c23da87e4d11404a4dff`，新连接为 `d2adff022716f9cdfa4d53ade0bb7d87`。策略最终撤回回执为 revision 3、applied。

第一次较窄用例在 `build/native-runtime-20261117/acceptance.log`，101.54 秒通过。之后加强回执故障、带连接崩溃及非空旧 runtime 注入，以上最终运行覆盖加强后的代码。最初探针链接因未链接访问票据实现失败，补齐 `chast.host` 依赖后构建通过；未将该失败构建记为成功。

补充验证（`server` 目录）：

```powershell
go test ./internal/api -run 'LiveConnection|ClientStrategy' -count=1 -timeout 3m
go vet ./internal/api
```

均通过；定向测试耗时 3.569 秒，日志位于 `build/native-runtime-20261117/go-focused.log`、`go-vet.log`。`git diff --check` 通过。

日志中的 `proxy error: context canceled` 出现在故意终止/重启代理的阶段，测试没有失败。结束后确认本轮 host/tunnel/probe 进程均为 0；临时服务由测试清理。测试产物留在 build 目录。没有重跑上一轮 Node 45 项、Go 全量、PostgreSQL、Flutter 或原生全量 CTest，不把历史结果计作本轮新结果。

## 4. 后续复跑

已配置好 CMake 构建树的 Windows 开发机执行：

```powershell
.\tools\test-native-runtime.ps1
# 使用其他已有构建树：
.\tools\test-native-runtime.ps1 -BuildDirectory D:\path\to\build
```

脚本构建当前代码，每次创建独立产物目录，运行后恢复原有环境变量。用例在未设置 `CHAST_NATIVE_BIN_DIR` 时明确跳过，默认 `go test` 不会启动桌面主机。`CHAST_NATIVE_ARTIFACT_DIR` 用于保留运行目录；未设置则使用自动清理的测试目录。二进制清单见脚本。清理只针对测试启动并持有的 PID 和子进程树。

## 5. W1 后续工作

1. 第二台可连接 Windows 测试机上运行真实查看端，与本机主机组成跨机拓扑；执行同一主机两个查看端的精确断开、非目标会话持续收图和实际操作验证。
2. 扩展 host/viewer 逐键默认/锁定/撤回、会话快照与真实查看端独立回执；补实际三连接上限、未知键及下一会话语义。
3. 在明确的服务验收环境验证 SCM 服务/worker 启停、账号切换和安全桌面；本轮便携托盘不能替代服务路径。
4. 验证真实链路断网、重连、跨 NAT/代理、旧回执重放及较长故障窗口；本轮 503 故障只覆盖 HTTP 控制面。
5. 最后汇总 W1 缺口，执行发布所需完整门禁；生产数据库、灾备及性能对照继续按既定后续阶段安排。

当前环境没有已连接的第二台 Windows 验收主机，跨机项仍标待验收；不提前宣称 W1 发布完成或转入 C01/P2。
