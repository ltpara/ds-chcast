# RustDesk 缺口待办任务清单（2026-11-01）

依据 `docs/rustdesk-server-parity-2026-10-26.md` 与
`docs/status-2026-10-25.md` 的“已知限制 / 与 RustDesk 的剩余缺口”，
把剩余工作拆成可执行、可验收的任务。服务端 hbbs/hbbr 的代码级基线已无
缺失项，本清单不再重复已完成能力。

判定口径：

- **ENV**：功能已实现，缺真实环境证据；需要第二台机器、公网、路由器、
  摄像头或特定硬件。
- **DEV**：代码层面仍需开发，可在本机完成并附带自动化测试。
- **PLAT**：受驱动签名或非 Windows 平台前置限制，暂缓但保留任务定义。

优先级：P0 = 影响发布可信度或安全边界；P1 = 对齐与运维增强；
P2 = 平台扩展。

## 任务总览

| ID | 优先级 | 任务 | 类型 | 依赖 | 规模 |
| --- | --- | --- | --- | --- | --- |
| ENV-01 | P0 | 双机跨 NAT 打洞与中继回退验收 | 验收 | 两台机器、公网服务器 | M |
| ENV-02 | P0 | 真实跨网 IPv6 双栈验收 | 验收 | IPv6 出口 | S |
| ENV-03 | P0 | 真实路由器 UPnP 与对称 NAT 验收 | 验收 | 可管理路由器 | M |
| ENV-04 | P0 | 双机音频可听验收与自适应默认值决策 | 验收 | 两台机器、可听环境 | M |
| ENV-05 | P0 | 声卡/端点热插拔与 AEC 验收 | 验收 | 可插拔音频设备 | S |
| ENV-06 | P1 | 摄像头媒体路径验收 | 验收 | USB/内置摄像头 | S |
| ENV-07 | P1 | HEVC 主机→查看器真实会话验收 | 验收 | HEVC 编码硬件 | S |
| ENV-08 | P1 | 多显示器、负 origin 与高 DPI 真机验收 | 验收 | 双屏机器 | S |
| ENV-09 | P1 | 终端/文件/端口转发/聊天双机交互矩阵 | 验收 | 两台机器 | M |
| ENV-10 | P1 | WebAuthn 真实弹窗与免密码登录验收 | 验收 | Windows Hello/FIDO2 | S |
| ENV-11 | P1 | 英文布局截图复核（审批/聊天/托盘） | 验收 | Windows 桌面 | S |
| ENV-12 | P1 | 真实 CA、ACME 与 CDN/反代载体验收 | 验收 | 域名、公网、企业 CA | L |
| DEV-01 | P0 | 更新发布者签名与启动回滚 | 开发 | 无 | M |
| DEV-02 | P1 | 数据面 tracing 与队列/SQLite 指标 | 开发 | 无 | M |
| DEV-03 | P1 | PostgreSQL 后端与迁移对齐 | 开发 | 本机容器或 PG 实例 | L |
| DEV-04 | P1 | 账号按组授权与配额 | 开发 | 无 | L |
| DEV-05 | P1 | 连接审计 GeoIP 与路由关联 | 开发 | GeoIP 数据（离线） | M |
| DEV-06 | P1 | 审计归档集中去重与迟到合并 | 开发 | 无 | M |
| DEV-07 | P1 | 主机自报元数据签名 | 开发 | 无 | S |
| DEV-08 | P2 | 地址簿拼音/模糊搜索与搜索词持久化 | 开发 | 无 | S |
| DEV-09 | P2 | 更多采集/编码后端 | 开发 | 对应硬件 | L |
| DEV-10 | P2 | 跨子网/中继协助 Wake-on-LAN | 开发 | 无（超出 RustDesk） | M |
| DEV-11 | P2 | SOCKS5 代理（已完成 2026-11-02） | 开发 | 无 | M |
| PLAT-01 | P2 | 虚拟显示器（01a/01c 完成，01b 脚手架完成，真机安装阻塞） | 平台 | 驱动再分发许可；无头测试机 | L |
| PLAT-02 | P2 | 远程打印 | 平台 | v4 打印机驱动签名 | L |
| PLAT-03 | P2 | 非 Windows 平台与打包 | 平台 | CI/平台环境 | XL |

## A. 真实环境验收（ENV）

### ENV-01 双机跨 NAT 打洞与中继回退（P0）

- 目标：证明真实 NAT 下“优先直连、失败回退中继、网络切换后恢复”成立。
- 实施：公网服务器同时启动 `chast-server` 与 `chast-relay`（TLS）；
  主机 A 位于家庭 NAT 后，查看器 B 位于另一网络；连续连接 10 次；
  在连接中切换 A 的网络（Wi-Fi↔有线或 4G 热点）观察重连。
- 验收：10 次中直连成功 ≥8 次，其余在中继上建立；文件上传/下载
  SHA-256 与源文件一致；终端、端口转发、聊天各完成一次；
  断网重连 <5 s 或按客户端重试策略恢复；日志能区分 punch 与 relay。
- 证据：保存客户端日志摘要、连接耗时、文件摘要与重连时间到
  `docs/status-2026-10-25.md` 新增小节。

### ENV-02 真实跨网 IPv6 双栈（P0）

- 目标：验证 AAAA 解析、v6-only 会合、v4 回退与 v6 中继。
- 实施：给服务器配置 IPv6 与 AAAA；分别在仅 v6、仅 v4、双栈网络
  各连接 5 次；关闭 v6 路由后确认自动回退 v4。
- 验收：v6-only 5/5 成功；v4-only 5/5 成功；双栈优先 v6；
  拔掉 v6 后下一次连接走 v4；无 30 s 以上卡顿。
- 证据：`nslookup`/日志中的地址族、连接结果与耗时。

### ENV-03 真实路由器 UPnP 与对称 NAT（P0）

- 目标：验证 `--upnp` 建立映射、续租、失败清理，以及对称 NAT 下
  直连确实失败并回退中继。
- 实施：开启路由器 UPnP；租约周期内等待至少两次续租；
  重启路由器后确认映射重建；在对称 NAT 网络重复连接。
- 验收：WAN 侧可见映射；续租无中断；路由器重启后自动重建；
  对称 NAT 下没有“假直连”，中继会话可用。
- 证据：路由器映射截图/日志、续租时间戳、失败回退记录。

### ENV-04 双机音频可听验收与自适应默认值（P0）

- 目标：决定 `--adaptive-jitter` 是否默认开启，并验证 PLC 可听效果。
- 实施：双机 30 分钟会话，播放音乐与语音；分别在洁净链路和
  `chast-netem` 弱网档运行自适应开/关；记录 skew、`starved_ms`、
  丢块与可听卡顿；人工试听丢包隐藏。
- 验收：洁净链路无爆音/断音；弱网下自适应实际静默低于基线；
  A/V skew 不劣于基线；给出“默认开/默认关”的书面结论与理由。
- 证据：两档指标表、试听记录、默认值变更（若开）及对应文档。

### ENV-05 声卡/端点热插拔与 AEC（P0）

- 目标：验证默认设备切换 1 s 内跟随、5 s 启动确认重建与回声消除。
- 实施：会话中切换默认麦克风/扬声器；拔出再插回 USB 耳机；
  使用外放验证 AEC；观察是否重复或丢设备。
- 验收：端点切换后 1 s 内恢复；空闲端点不触发无谓重建；
  外放无持续回声；日志无卡死或重复采集。
- 证据：设备切换时间线与听感记录。

### ENV-06 摄像头媒体路径（P1）

- 目标：验证真实摄像头枚举、采集、硬件编码、查看器解码与停止。
- 实施：带摄像头机器上打开/切换/停止摄像头；撤销权限；
  连接中拔插设备。
- 验收：至少 3 帧 CRC 正确；停止后收到 FIN；撤销权限立即结束；
  拔插后能重新枚举且不拖慢桌面流。
- 证据：帧计数、错误日志与桌面流帧率对比。

### ENV-07 HEVC 真实会话（P1）

- 目标：验证主机 HEVC 编码→查看器解码→录制 hvc1 的完整链路。
- 实施：使用支持 HEVC 编码的主机，开启查看器录制；中途切换分辨率。
- 验收：查看器正常解码；录制分段且 `hvc1` 可被 FFmpeg 读回；
  样本数与接收访问单元一致。
- 证据：编码器名称、段数与样本校验输出。

### ENV-08 多显示器与高 DPI（P1）

- 目标：验证负 origin、跨屏、不同缩放下的输入映射与画面选择。
- 实施：双屏（主屏在右、缩放不同）切换显示器；在边界点击与拖拽。
- 验收：点击落点误差 ≤1 px 或系统 DPI 允许误差；负坐标不越界；
  切换后无需重连。
- 证据：落点截图/坐标记录。

### ENV-09 双机功能矩阵（P1）

- 目标：一次性覆盖终端、文件、端口转发、聊天、剪贴板与截图。
- 实施：按矩阵逐项执行并勾选；包含取消上传、取消下载与权限拒绝。
- 验收：全部通过；错误路径有明确提示且会话可继续。
- 证据：矩阵表附结果与失败项修复记录。

### ENV-10 WebAuthn 真机（P1）

- 目标：验证 Windows Hello/FIDO2 弹窗、密码+断言与免密码断言。
- 实施：注册安全密钥、两种登录入口、移除密钥、恢复码回退。
- 验收：真实弹窗可用；失败提示准确；移除后不能再登录。
- 证据：弹窗截图与审计记录。

### ENV-11 英文布局截图（P1）

- 目标：补齐审批窗、聊天窗、托盘菜单的英文/中文布局复核。
- 实施：100% 与 150% 缩放下截图对比，检查截断与换行。
- 验收：无截断、无重叠；必要时调整最小宽度。
- 证据：截图归档到 `docs/status-2026-10-25.md` 对应小节。
- 状态（2026-11-02）：审批窗、聊天窗与托盘菜单的中英文截图已归档到
  `docs/status-2026-10-25.md` §6.52；主窗、审批窗、聊天窗与托盘均无
  截断/重叠。复核发现审批窗“接收目录”只占第二列，默认路径显示为
  `C:\Users\...`，已在 `host/main.cc` 改为占满对话框剩余宽度并重建复测，
  中英文都能完整显示默认 `C:\Users\admin\Downloads\chast`。本机为 100%
  缩放；主机是 DPI 不感知进程（无清单、无感知 API），150% 下由系统整窗
  位图缩放、逻辑布局不变，因此没有 150% 截图，真实高 DPI 重排归 ENV-08。
- 截图文件：`build/env11-en-main/chat/approval/tray.png` 与
  `build/env11-zh-*.png`（本机，未纳入版本库）。

### ENV-12 真实 CA、ACME 与 CDN/反代（P1）

- 目标：验证企业 CA 链、公网 ACME 与共享载体经反代/CDN。
- 实施：临时公网域名走 HTTP-01；企业 CA 签发 mTLS；反代
  `/carrier/chast` 前缀；热替换证书。
- 验收：证书链校验通过；续期无需重启；载体经反代可用；
  CDN 场景下延迟与断线重连符合预期。
- 证据：证书链、续期日志、反代配置与连接结果。

## B. 本机开发（DEV）

### DEV-01 更新发布者签名与启动回滚（P0，2026-11-02 完成）

- 目标：更新包同时校验服务器签名与发布者签名，升级失败可回滚。
- 实施：发布清单增加 `publisher_key_id` 与 `publisher_sig`（Ed25519，
  对版本/URL/SHA-256 签名）；`chast-server --software-publisher-key`
  与签名工具；主机代理与 `chast-tunnel apply-update` 双重校验；
  新版本启动写入健康标记，超时未标记则回滚到上一版本。
- 验收：篡改清单/替换文件被拒；未签名包在强制模式下被拒；
  健康检查失败自动回滚；正常升级与回滚各一次真实进程回环。
- 测试：Go 单测覆盖签名与清单解析；C++/Dart 更新路径单测；
  回环脚本断言两种失败分支。
- 文件：`server/internal/software/`、`server/internal/agent/host.go`、
  `server/internal/updater/`、`tools/` 下的更新工具。
- 状态：已完成。发布者清单与 `chast-release` 工具、
  `--software-publisher-key`/`--software-manifest` 启动校验与字段转发、
  host/apply-update 的 `--publisher-pubkey` 固定、健康标记与超时回滚
  （`confirm-update`）均已落地。验证见 `docs/status-2026-10-25.md` §6.45：
  单测覆盖篡改与固定；Windows 真实进程集成测试
  `TestApplyUpdateWatchdog` 覆盖正常升级、早退/超时回滚、确认标记与错误
  pin；真实 `chast-server` 回环确认发布者字段下发与篡改清单启动拒绝。
  边界：主机 `--auto-update` 启动厂商安装器，不参与回滚；发布者固定为
  显式可选，未配置时保持只校验服务器签名。

### DEV-02 数据面 tracing 与队列/SQLite 指标（P1，2026-11-02 完成）

- 目标：让中继/会合数据面、SQLite 与队列可观测。
- 实施：在 punch、relay 会话、QUIC 握手与流关闭处生成 span；
  记录 SQLite 查询耗时与队列深度；沿用现有 OTLP/HTTP JSON 导出，
  评估是否补 gRPC 传输。
- 验收：一次双机/回环会话可在 trace 中串起会合→打洞→中继→会话；
  慢查询与队列积压有指标；采样配置可热更新或明确重启生效。
- 测试：tracing 单测、指标断言与一次回环导出。
- 状态：已完成。`wire` 与载体 hello 的 `traceparent` 为增量字段（协议版本
  不变），查看者代理把 lookup 响应的上下文带入 `punch_request`、
  `relay_viewer` 与共享载体 hello；服务端生成 `punch.request`、
  `relay.viewer_auth`、`relay.udp_session`、`relay.carrier_session` span，
  UDP 会话与查看者鉴权同级挂在该查看者 lookup span 下。SQLite 通过
  `internal/sqlitex` 驱动级埋点按 `store`/`op` 暴露查询/事务耗时、错误、
  慢查询（≥50 ms）与单连接池等待；tracing 队列暴露深度与丢弃计数。验收由
  `TestLoopbackTraceCoversLookupPunchAndRelay` 覆盖一次回环的
  会合→打洞→中继→会话单 trace；采样在进程启动时读取，重启生效。
  边界：服务端只转发不透明 QUIC 数据报，无法生成 QUIC 握手/流 span，会话
  span 覆盖可观察的中继/载体生命周期；未引入 gRPC exporter；双机真实链路
  仍待 ENV-01。详见 `docs/status-2026-10-25.md` §6.47 与
  `server/README.md` 的 tracing/指标说明。

### DEV-03 PostgreSQL 后端（P1）

- 目标：在不破坏 SQLite 默认部署的前提下支持 PostgreSQL。
- 实施：抽出账号/地址簿/审计存储接口；用 `pgx` 实现；
  复用 `schemamig` 的版本化迁移并保证 SQLite/PG 语义一致；
  增加 `CHAST_TEST_POSTGRES_DSN` 门控的集成测试。
- 验收：同一套 API 在两者上通过；迁移从旧版本升级成功；
  单连接限制解除后可并发；部署文档给出连接串与迁移命令。
- 测试：`go test ./...` 全绿；PG 集成测试在可用实例上通过。

### DEV-04 账号按组授权与配额（P1，2026-11-02 完成）

- 目标：账号/设备按组授权，支持配额，而不仅是逐设备标签。
- 实施：新增组、成员与配额表；管理 API 与 Web 管理台；
  连接/审计接口按组过滤；与地址簿分组 ACL 解耦。
- 验收：组内只读/可写/管理员三档生效；超配额被拒并给出明确错误；
  最后一个管理员保护与现有角色兼容。
- 测试：Go 存储/HTTP 测试覆盖越权与配额边界；管理台回环。
- 文件：`server/internal/account/`（schema v5、`accessgroup.go`）、
  `server/internal/api/accessgroups.go`、`server/internal/webadmin/`。
- 状态：已完成。SQLite schema v5 新增组、成员与设备表；组角色
  `readonly`/`write`/`admin` 只在组内生效，server-key 与全局 admin 管理
  任意组。成员/设备超配额或配额下调低于当前用量返回 409 与当前用量；
  每组保护最后一个可用 admin，被封禁的 admin 不计入且可被移除。API 提供
  CRUD、成员、设备与组内连接/文件审计；组审计按 `host_id`/`peer_id` 匹配
  设备，组端点不支持 CSV。管理台新增“访问组”页签并在审计连接/文件子页
  支持按组过滤。验证见 `docs/status-2026-10-25.md` §6.48：存储与 HTTP
  单测覆盖角色、越权、配额边界、最后 admin 与审计隔离；真实回环在浏览器
  完成三档角色、配额拒绝、最后 admin 回滚、组审计过滤与 CSV 门控；
  `go vet`/`go build`/全量 `go test ./... -count=1` 通过。边界：组不影响
  查看器地址簿 UI；组配额限制分配数而非在线主机数。

### DEV-05 连接审计 GeoIP 与路由关联（P1，2026-11-02 完成）

- 目标：连接与文件传输审计带登录地点/路由，且能按 `audit_ref` 关联。
- 实施：复用 `internal/geoip` 为连接审计补地点；记录 punch/relay
  路由；查看器文件传输由轮询聚合改为逐文件/逐批可核对。
- 验收：审计查询能显示地点与路由；同一会话的连接/文件记录可关联；
  未配置 GeoIP 时只记录 IP。
- 测试：GeoIP 单测、审计合并单测与回环。
- 文件：`server/internal/account/`（schema v6、`connection.go`、
  `file.go`、`migrate.go`、`export.go`）、`server/internal/api/`、
  `server/internal/webadmin/assets/app.js`、`app/lib/pages/session_page.dart`、
  `app/lib/server_api.dart`、`app/windows/viewer/viewer_engine.*`、
  `app/windows/viewer/viewer_bridge.cc`、`tools/viewer_probe.cc`。
- 状态：已完成。SQLite schema v6 给 `connection_events` 与 `file_events`
  增加 `location`/`route`；服务端在写入时用客户端 IP 查可选 GeoIP 表并把
  地点落库，未配置时地点为空、IP 照常记录。查看器在连接与文件报告中携带
  当次实际路径 `direct`/`relay`/`carrier`（没有服务端时为 `direct`），
  非法值返回 400；同一会话的查看器与主机报告继续按 `audit_ref` 合并，
  地点/路由只由查看器侧非空值补齐，因此两侧记录可关联且不互相覆盖。
  审计查询、CSV 与管理台连接/文件表均显示地点与路由。查看器文件传输不再
  按轮询窗口聚合：引擎为每个完成的文件写入有界审计日志（保留最近 128 条，
  带 `first_seq`/`dropped` 水位），桥接后由页面逐条按 `seq` 上报
  （每条 `files=1`），会话 `audit_ref` 变化时重置游标，日志溢出时跳过缺口。
  验证见 `docs/status-2026-10-25.md` §6.49：GeoIP/迁移/合并/API 单测；
  真实 `chast-server` 回环在配置 `127.0.0.1/32 GeoLab-Loopback` 时查询到
  地点 `GeoLab-Loopback`、路由 `relay` 且连接/文件共享同一 `audit_ref`，
  无 GeoIP 服务只显示 IP、地点为空，非法路由被 400 拒绝；C++ 探针断言
  逐文件日志覆盖三文件上传成功、上传取消与下载取消。边界：地点只来自
  离线表、不做在线查询；路由是查看器上报时的实际路径，主机侧单独记录
  没有地点/路由；逐文件日志只保留最近 128 条，更早记录只有计数。

### DEV-06 归档集中去重与迟到合并（P1，2026-11-02 完成）

- 目标：远端投递不重复，迟到的主机审计能并入对应日归档。
- 实施：投递账本按“日期+内容哈希”去重；迟到记录按 `occurred_at`
  合并到已关闭日文件（重写受控或追加旁路索引）。
- 验收：重复投递只成功一次；迟到记录可在归档查询到；
  失败重试不产生重复。
- 测试：本地 HTTP/S3 模拟端点单测与归档查询断言。
- 文件：`server/internal/auditarchive/`（`archive.go`、`archive_test.go`、
  `remote_test.go`）、`server/internal/admin/account.go`、
  `server/internal/admin/account_test.go`、`server/cmd/chast-server/main.go`、
  `server/README.md`。
- 状态：已完成。归档行按事件自身的 UTC 日期落文件：账号审计用
  `created_at`、连接用 `started_at`（缺失回退 `ended_at`）、文件传输用
  `occurred_at`；缺失或未来日期归当天。当天数据照旧追加；已关闭日期
  （早于当天）的数据追加到该日文件内的受控重写：读入现有明文或 gz、
  追加行、写临时文件并 fsync 后原子改名（`--audit-archive-gzip` 时保
  持 gz），删除另一格式与两种 `.delivered` 标记，使被改写内容必然重投。
  投递去重改为持久账本 `delivery-ledger.json`（归档目录内），键为
  `<UTC 日期>-<内容 SHA256>`，按投递目标（远端 URL 或 S3 bucket URL）
  分域；损坏账本记警告并从空开始。账本命中时不重传、直接恢复标记；
  成功投递后原子保存账本，进程重启后仍生效。`Idempotency-Key` 由文件名
  改为内容 SHA-256，因此同日文件被迟到合并改写后会作为新内容重投，
  而同一内容的重试仍被去重。`Result.Merged` 统计迟到合并行数，管理台
  `audit archive run` 输出 `merged=N`，服务器日志同步记录。检查点语义
  不变：先写文件并 fsync，再推进归档检查点；崩溃重放可能重复行，由
  消费端按流+ID 去重。验证见 `docs/status-2026-10-25.md` §6.50：
  `auditarchive` 单测覆盖迟到合并（明文与 gz）、重复投递、标记丢失后
  账本去重、换目标重投与账本损坏恢复；真实 `chast-server` 回环把两天前
  的连接报告合并进 `connections-20261031.jsonl.gz`，HTTP 模拟端点只收到
  一次 PUT（摘要等于 `X-Chast-Audit-Sha256` 与 `Idempotency-Key`），
  后续轮次不重传、账本仅一条，重启前删除投递标记后账本仍跳过上传并
  恢复标记。边界：S3 走同一 HTTP PUT 路径（预签名/兼容端点），未做
  分片上传；账本按目标 URL 分域，修改投递 URL 视为新目标并重投；
  归档查询仍直接读文件，不提供跨日搜索索引。

### DEV-07 主机自报元数据签名（P1，2026-11-02 完成）

- 目标：主机注册/lookup 元数据不可被中间人篡改。
- 实施：主机用绑定私钥签名元数据摘要；服务器验签后落库；
  旧客户端无签名时按兼容模式放行并可配置强制。
- 验收：篡改元数据被拒；兼容模式不破坏旧主机；管理台显示签名状态。
- 测试：协议/存储单测与回环。
- 文件：`server/internal/identity/`、`server/internal/api/`、
  `server/internal/registry/`、`server/internal/agent/client.go`、
  `server/internal/webadmin/`。
- 状态：已完成。新增 `chast-host-metadata-v1` 域，
  `HostMetadataMessage` 对 `id`、`hostname`、`platform`、`version` 与注册
  时间戳做长度前缀规范编码；主机代理在 `POST /v1/hosts` 的 `metadata_sig`
  中携带签名，服务器在任何注册表变更前用本次提交的主机公钥验签。缺签名
  默认放行（旧客户端），签名存在但无效/被篡改/属于其他 ID 一律 401；
  `--require-host-metadata-sig` 拒绝无签名注册。验签结果按身份持久化为
  `metadata_signed`（SQLite schema v2，`schemamig` 增量迁移），
  `/v1/identities` 与 Web 管理台设备表直接显示。验证见
  `docs/status-2026-10-25.md` §6.46：identity/registry/api/agent/webadmin
  单测覆盖篡改、外域 ID、兼容模式、强制模式、迁移升级与重启持久化；
  真实 `chast-server` 回环在强制模式下确认无签名/篡改/外 ID 均 401、
  正确签名 200，`/v1/identities` 返回 `metadata_signed:true` 且重启后保持；
  兼容模式（无旗标）下无签名旧主机注册 200 且 `metadata_signed:false`。
  边界：签名只覆盖展示元数据与注册时间戳，不覆盖候选/NAT/中继租约等
  路由字段（这些仍由 keepalive/relay 一次性挑战保护）；`metadata_signed`
  记录最近一次注册的状态，旧主机后续无签名续约会把状态清回 false。

### DEV-08 地址簿拼音/模糊搜索与持久化（P2，2026-11-02 完成）

- 目标：补齐搜索体验细节（非对齐缺口）。
- 实施：拼音首字母与子串模糊匹配；搜索词按视图持久化到
  `ui-filter.json`，清空即删除。
- 验收：中文/英文/拼音均可命中；“我的设备”与共享对话框互不覆盖；
  重启后恢复。
- 测试：Dart 单测与 widget 测试。
- 文件：`app/lib/search_match.dart`、`app/lib/pinyin_initials.dart`、
  `app/lib/view_filter.dart`、`app/lib/pages/home_page.dart`、
  `app/lib/pages/shared_books_dialog.dart`、`app/test/search_match_test.dart`、
  `app/test/view_filter_test.dart`、`app/test/home_page_test.dart`、
  `app/test/shared_books_dialog_test.dart`、`tools/gen_pinyin.py`（生成器）。
- 状态：已完成。匹配规则为大小写不敏感子串，另加每个字段的拼音首字母
  串：中文按 GB2312 一级字序取主读音首字母，ASCII 字母数字保留小写，
  空白与标点丢弃。因此“我的电脑”与“我的 电脑”都能被 `wddn` 命中，
  “gzspc”能命中“工作室 PC”。首字母表由 `tools/gen_pinyin.py` 离线生成
  （Python 内置 GB2312 编解码，3755 个常用字、23 个声母分组），不引入
  第三方依赖、不复制任何拼音库数据；GB2312 之外的生僻字只参与字面
  匹配。接入“我的设备”的设备名/ID/标签/别名/主机标签与分组名（含祖先
  分组）匹配，以及共享地址簿对话框的 ID/名称/标签匹配。搜索词按视图
  持久化到 `%APPDATA%\chast\ui-filter.json`：`deviceSearch` 与
  `sharedSearch` 两个独立键，与标签、折叠状态共用同一文件且互不覆盖；
  输入即保存（去首尾空白），清空搜索框或重置筛选即删除该键，启动时
  恢复并回填搜索框。验证见 `docs/status-2026-10-25.md` §6.51：匹配器/
  首字母表与 ViewFilter 单测；widget 测试覆盖拼音命中设备、重启恢复、
  共享对话框拼音命中/恢复/清空且不影响设备页；`flutter analyze` 无问题、
  全量 `flutter test` 通过。边界：只支持首字母而非全拼；多音字按 GB2312
  主读音（如“重”为 z、“长”为 c）；生僻字与非 ASCII 拉丁/西里尔字母只做
  字面匹配；搜索词保存在本地文件，不随账号同步。

### DEV-09 更多采集/编码后端（P2）

- 目标：按硬件能力扩展 NVENC/QSV/AMF 或 Media Foundation 硬件编码。
- 实施：在 `src/codec/encoder_factory.cc` 增加运行时探测与回退；
  记录实际后端；保持 HEVC 录制 hvcC 路径一致。
- 验收：有对应硬件的机器优先硬件编码，失败回退软件；
  码率/延迟不劣于现有后端。
- 测试：工厂单测（无硬件走回退）；硬件机器回环实测。

### DEV-10 跨子网/中继协助 Wake-on-LAN（P2，2026-11-02 完成）

- 目标：超出 RustDesk 的增强，支持跨子网唤醒。
- 实施：服务器/同网段代理代发魔术包；鉴权与防滥用；可选审计。
- 验收：跨子网可唤醒；未授权请求被拒；与现有同网段直发共存。
- 说明：RustDesk 同样只支持同网段，本项不计入对齐缺口。
- 文件：`server/internal/wake/wake.go`、`server/internal/wol/`、
  `server/internal/api/wake.go`、`server/internal/agent/client.go`、
  `server/internal/agent/host.go`、`server/cmd/chast-tunnel/main.go`、
  `server/internal/api/wake_test.go`、`server/internal/agent/wake_relay_test.go`、
  `app/lib/server_api.dart`、`app/lib/pages/home_page.dart`、
  `app/test/server_api_test.dart`、`app/test/home_page_test.dart`。
- 状态：已完成。服务端内存队列与 `POST /v1/hosts/{id}/wake`：请求体只有
  `mac`，接受 server-key、账号令牌或开放的服务器；非管理员必须能在自己的
  地址簿或任一共享地址簿中找到目标。每目标 30 s 冷却（429 +
  `Retry-After`），队列上限 64 条，任务 60 s 过期，成功/拒绝均写入
  `device_wake` 审计。主机注册响应增量携带 `wake` 数组，目标自身不领取
  自己的任务，同一代理不会重复领取；服务器重启会丢弃未领取任务。
  `chast-tunnel host --wake-relay [--wake-targets addr:port,...]` 默认关闭，
  开启后在目标所在链路广播 6 个 FF 加 16 份 MAC 的 102 字节魔术包，默认
  目标为受限广播加各网卡定向广播。查看器在已登录同一连接服务时额外提供
  “通过连接服务转发”复选框，默认仍是本地直发，原有 MAC 持久化不变。
  验证：Go 单测覆盖投递/去重/过期/冷却/队列满、鉴权与校验（含共享地址
  簿授权与撤销）、真实回环 UDP 包字节；真实 `chast-server` + 两个代理回环中
  202 后 3.65 s 由代理刷新
  领取并发出唯一 102 字节包，匿名 401、坏 MAC 400、未知主机 404、冷却 429；
  另以管理台创建普通账号 `viewer` 并登录，未入簿主机 403、PATCH 地址簿后
  202 且由代理实际发出 102 字节包，审计记录 `device_wake`；
  `go vet ./...`、全量 `go test ./... -count=1`、`flutter analyze` 与全量
  `flutter test`（531 项）通过。边界：队列只在内存；代理只在注册刷新时
  领取，最坏延迟约为注册 TTL/2；未在真实网卡/主板上验证开机与定向广播的
  防火墙放行。

### DEV-11 SOCKS5 代理（P2，2026-11-02 完成）

- 目标：与 RustDesk 的 `socks5` 配置对齐，让会合/中继的 TCP 与 HTTP API
  经代理，UDP 路径保持直连。
- 实施：服务 HTTP API 与 TCP 载体（中继回退、共享载体）走
  `socks5://[user:pass@]host:port`，远端 DNS；主机/查看器的设置与
  `chast-tunnel` 参数接线；Dart 查看器实现 RFC 1928/1929 并接管
  `HttpClient.connectionFactory`（含 https）。
- 验收：带认证代理回环记录到 `CONNECT ...:18443` 与 `CONNECT ...:18445`；
  查看器 `path=carrier`；`--mode direct` 与不可达代理失败关闭；
  Go/Flutter/C++ 测试通过。详见 `docs/status-2026-10-25.md` §6.58。
- 边界：不实现 UDP ASSOCIATE；真实企业代理、双机与代理断开重连待验收。
- 文件：`server/internal/netproxy/`、`server/internal/agent/client.go`、
  `server/internal/agent/host.go`、`server/cmd/chast-tunnel/main.go`、
  `app/lib/socks5.dart`、`app/lib/server_api.dart`、`app/lib/server_settings.dart`、
  `app/lib/tunnel_agent.dart`、`app/lib/pages/home_page.dart`、
  `host/main.cc`、`include/chast/host_settings.h`、`app/test/socks5_test.dart`。

## C. 平台与驱动（PLAT）

### PLAT-01 虚拟显示器（P2）

- 目标：无物理显示器时提供虚拟桌面。
- 选型与接入设计：`docs/plat-01-virtual-display-2026-11-03.md`（第一版采用
  Amyuni usbmmidd_v2；自研 WHQL 驱动留作 ARM64/企业长期路线）。
- 前置：驱动再分发许可确认；实机无头测试环境。
- 实施：驱动安装/卸载、分辨率枚举、与现有采集/输入映射对接。
- 验收：未接显示器的主机可远程操作；卸载后系统恢复；驱动签名有效。
- 进度：PLAT-01a 已完成（2026-11-03）：host.ini 配置键、provider 接口、
  只读检测（未安装/已安装/运行中/不支持/冲突）、托盘与高级设置状态行、
  模式选择单测；未安装驱动。PLAT-01c 的策略层同日完成：off/auto/on 决策、
  worker 启动接线、托盘策略/数量/分辨率 UI 与假 provider 单测。PLAT-01b 的
  代码脚手架同日完成：manifest/SHA-256/Authenticode 包校验、状态文件、
  `--vdisplay` 提权 CLI、deviceinstaller64 安装/卸载/数量/模式动作与 106 项
  单测，最终源码全量 ctest 42/42（640.11 s，2026-11-03）。仍缺固定哈希驱动包、
  Amyuni 再分发许可、第三方声明、安装器接线与 A1～A10 真机验收，因此
  PLAT-01b 未完成。后续 PLAT-01b～e 见设计文档 §9。

### PLAT-02 远程打印（P2）

- 目标：查看器把文档打印到本地打印机。
- 前置：签名 v4 打印机驱动（INF + 适配 DLL）与安装权限。
- 实施：主机安装虚拟打印机，`StartXpsPrintJob` 输出，查看器侧接收。
- 验收：文本/图片文档打印成功；卸载干净；无签名警告。

### PLAT-03 非 Windows 平台与打包（P2）

- 目标：至少补齐 Linux 查看器，再评估 Linux 主机与其他平台。
- 实施：拆分平台相关采集/输入/音频层；CI 构建与包格式
  （deb/rpm/AppImage）；文档说明能力差异。
- 验收：Linux 查看器可连接 Windows 主机并完成画面/输入/文件；
  打包产物可安装；测试矩阵记录已知限制。

## 推荐顺序

1. **ENV-01、ENV-02、ENV-04**：先消除发布可信度风险，直接决定
   默认值与对外承诺。
2. **DEV-05、DEV-06**：运营能力对齐（DEV-01/02/04/07 已完成）。
3. **DEV-03**：等 PostgreSQL 实例可用后再做存储扩展；DEV-02 的可观测性
   已落地。
4. **ENV-05~ENV-12**：按硬件/网络条件穿插执行，每项完成后在
   `docs/status-2026-10-25.md` 增补证据小节。
5. **DEV-09**：体验与增强项（DEV-08、DEV-10 已完成）。
6. **PLAT-01~PLAT-03**：等签名证书与平台资源就绪后启动。

## 环境阻塞与解锁条件（2026-11-02 实测）

本机为 Windows 10 x64、单显示器（1920×1080、AppliedDPI=96）、
Intel HD Graphics 630（驱动 31.0.101.2141），无摄像头；`psql`、
`docker`、PostgreSQL 服务与当前用户代码签名证书均不存在。以下项目
无法在本机完成真实验收，解锁条件如下：

| 项目 | 实测阻塞 | 解锁条件 |
| --- | --- | --- |
| ENV-01/03/09 | 仅单机，无真实路由器/NAT 对端 | 第二台机器与可配置 UPnP/对称 NAT |
| ENV-02 | 无跨网 IPv6 环境 | 两端公网 IPv6 或可路由测试网 |
| ENV-04/05 | 有 Realtek/USB 音频端点，但需双机试听、物理拔插与外放 AEC | 双机、可听环境、可插拔 USB 音频 |
| ENV-06 | 无 Camera 类在线设备 | 接入真实摄像头 |
| ENV-07 | HD 630 的 HEVC 探测声称可用，实际编码回退 H.264；`--encoder ffmpeg --codec hevc` 因 QSV 插件加载失败无法建编码器 | 支持 HEVC 编码的 GPU/驱动（NVENC、新 QSV 或 AMF） |
| ENV-08 | 单显示器、100% 缩放 | 双屏且缩放不同（含负 origin） |
| ENV-10 | 需人工完成 Windows Hello/FIDO2 弹窗与真实密钥 | 可用平台认证器或 FIDO2 密钥 |
| ENV-12 | 无公网域名、CA 账户与反代/CDN | 真实域名、CA 与 CDN/反代环境 |
| DEV-03 | 无 PostgreSQL 实例、容器运行时或服务 | 提供 PostgreSQL 实例 |
| DEV-09 | 仅 Intel HD 630；无 NVIDIA/AMD；HEVC 不可用 | NVENC/AMF 或支持 HEVC 的硬件 |
| PLAT-01/02 | 无代码签名证书（CurrentUser 0 张） | EV 证书与驱动签名流程 |
| PLAT-03 | 无 macOS/Linux 构建宿主 | Linux 构建环境 |

HEVC 探测（`chast-encode --codec hevc --seconds 1`）日志为
`encoder fell back: requested HEVC, producing H.264`；ffmpeg 路径报
`hevc_qsv ... Error loading plugins` 与 `avcodec_open2 failed`。

## 完成定义

每个任务完成时必须同时满足：

- 代码与自动化测试落地，或验收证据（日志、截图、指标）归档；
- `roadmap.md` 增补对应编号条目（纯验收任务写结论与证据位置）；
- `status-2026-10-25.md` 增补对应小节与验证命令/结果；
- 涉及协议或 C++ 时重跑串行 `ctest`，纯 Dart/Go 切片按既有流程验证；
- 明确写出边界（未覆盖场景、默认值、前置条件）。

## 参考

- `docs/rustdesk-server-parity-2026-10-26.md`
- `docs/status-2026-10-25.md`（“已知限制 / 与 RustDesk 的剩余缺口”）
- `docs/roadmap.md`（已完成切片与“明确不做”）
