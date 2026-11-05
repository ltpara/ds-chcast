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
| PLAT-01 | P2 | 虚拟显示器 | 平台 | 驱动签名证书 | L |
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

### ENV-12 真实 CA、ACME 与 CDN/反代（P1）

- 目标：验证企业 CA 链、公网 ACME 与共享载体经反代/CDN。
- 实施：临时公网域名走 HTTP-01；企业 CA 签发 mTLS；反代
  `/carrier/chast` 前缀；热替换证书。
- 验收：证书链校验通过；续期无需重启；载体经反代可用；
  CDN 场景下延迟与断线重连符合预期。
- 证据：证书链、续期日志、反代配置与连接结果。

## B. 本机开发（DEV）

### DEV-01 更新发布者签名与启动回滚（P0，2026-11-05 完成）

- 目标：更新包同时校验服务器签名与发布者签名，升级失败可回滚。
- 实施：发布清单增加 `publisher_key_id` 与 `publisher_sig`（Ed25519，
  对域串/密钥 ID/版本/URL/SHA-256/长度的长度前缀消息签名）；
  `chast-server --software-publisher-key` 启动时校验 `.pubsig` 边车并随
  通告下发；主机代理（`--publisher-pubkey`/`--require-publisher-signature`）
  与 `chast-tunnel apply-update`（`--publisher-pubkey`/`--publisher-sig`/
  `--require-publisher`）双重校验；新增 `chast-publish` 从 32 字节
  Ed25519 种子生成边车。查看器在新版本首帧后写入
  `CHAST_UPDATE_HEALTH_FILE` 健康标记，`apply-update` 看门狗在
  `--health-timeout`（默认 60 s）内未见到标记且进程已退出时恢复
  `.old-*` 备份并重启；机器安装器在替换前快照 `chast-host.exe`/
  `chast-service.exe`，新服务未能进入 Running 时恢复并重新启动。
- 验收：篡改清单/替换文件被拒；未签名包在强制模式下被拒；
  健康检查失败自动回滚；正常升级与回滚各一次真实进程回环。
- 测试：Go 单测覆盖签名与清单解析（`identity`、`updater`、
  `software`、`api`、`chast-publish`）；`CHAST_UPDATE_LOOPBACK=1`
  的 `TestUpdateLoopbackHealthyAndRollback` 用真实进程验证正常升级与
  崩溃回滚；`setup` 的 `TestInstallRestoresBinariesWhenExtractionFails`
  覆盖安装器恢复；查看器侧 `software_info_test.dart` 覆盖新字段。
- 文件：`server/internal/software/`、`server/internal/agent/host.go`、
  `server/internal/updater/`、`server/internal/setup/`、
  `server/cmd/chast-publish/`、`app/lib/tunnel_agent.dart`。

### DEV-02 数据面 tracing 与队列/SQLite 指标（P1）

- 目标：让中继/会合数据面、SQLite 与队列可观测。
- 实施：在 punch、relay 会话、QUIC 握手与流关闭处生成 span；
  记录 SQLite 查询耗时与队列深度；沿用现有 OTLP/HTTP JSON 导出，
  评估是否补 gRPC 传输。
- 验收：一次双机/回环会话可在 trace 中串起会合→打洞→中继→会话；
  慢查询与队列积压有指标；采样配置可热更新或明确重启生效。
- 测试：tracing 单测、指标断言与一次回环导出。

### DEV-03 PostgreSQL 后端（P1）

- 目标：在不破坏 SQLite 默认部署的前提下支持 PostgreSQL。
- 实施：抽出账号/地址簿/审计存储接口；用 `pgx` 实现；
  复用 `schemamig` 的版本化迁移并保证 SQLite/PG 语义一致；
  增加 `CHAST_TEST_POSTGRES_DSN` 门控的集成测试。
- 验收：同一套 API 在两者上通过；迁移从旧版本升级成功；
  单连接限制解除后可并发；部署文档给出连接串与迁移命令。
- 测试：`go test ./...` 全绿；PG 集成测试在可用实例上通过。

### DEV-04 账号按组授权与配额（P1）

- 目标：账号/设备按组授权，支持配额，而不仅是逐设备标签。
- 实施：新增组、成员与配额表；管理 API 与 Web 管理台；
  连接/审计接口按组过滤；与地址簿分组 ACL 解耦。
- 验收：组内只读/可写/管理员三档生效；超配额被拒并给出明确错误；
  最后一个管理员保护与现有角色兼容。
- 测试：Go 存储/HTTP 测试覆盖越权与配额边界；管理台回环。

### DEV-05 连接审计 GeoIP 与路由关联（P1）

- 目标：连接与文件传输审计带登录地点/路由，且能按 `audit_ref` 关联。
- 实施：复用 `internal/geoip` 为连接审计补地点；记录 punch/relay
  路由；查看器文件传输由轮询聚合改为逐文件/逐批可核对。
- 验收：审计查询能显示地点与路由；同一会话的连接/文件记录可关联；
  未配置 GeoIP 时只记录 IP。
- 测试：GeoIP 单测、审计合并单测与回环。

### DEV-06 归档集中去重与迟到合并（P1）

- 目标：远端投递不重复，迟到的主机审计能并入对应日归档。
- 实施：投递账本按“日期+内容哈希”去重；迟到记录按 `occurred_at`
  合并到已关闭日文件（重写受控或追加旁路索引）。
- 验收：重复投递只成功一次；迟到记录可在归档查询到；
  失败重试不产生重复。
- 测试：本地 HTTP/S3 模拟端点单测与归档查询断言。

### DEV-07 主机自报元数据签名（P1，2026-11-05 完成）

- 目标：主机注册/lookup 元数据不可被中间人篡改。
- 实施：主机在注册时用绑定私钥对 `id`、`hostname`、`platform`、`version`
  的长度前缀消息签名（域串 `chast-host-metadata-v1`），随
  `metadata_signature` 一并提交；服务器在注册签名之后验签，默认兼容
  旧主机，`chast-server --require-host-metadata-signature` 可强制要求。
  元数据先规范化再落库：只有规范化未改变上报字符串时才保留签名，
  否则丢弃，避免签名与 lookup 返回值不一致。带元数据的续租会替换
  签名（无签名的续租即视为撤销），不带元数据的续租保留上次报告。
  lookup 与批量 lookup 回显 `metadata_signature`；管理 CLI 的 `peers`
  显示 `metadata=signed|unsigned`，Web 管理台设备表新增“元数据签名”
  列。
- 验收：篡改元数据被拒；兼容模式不破坏旧主机；管理台显示签名状态。
- 测试：`identity` 覆盖域分离与字段边界；`registry` 覆盖签名跨重启
  持久化、无签名续租清除、规范化丢弃与重新签名；`api` 覆盖
  lookup 验签、篡改/错配签名 401、兼容与强制模式、规范化丢签名；
  `admin` 覆盖状态显示。`go build ./...`、`go vet` 与
  `go test ./internal/identity ./internal/registry ./internal/api
  ./internal/admin ./internal/agent ./internal/webadmin ./cmd/chast-server`
  全部通过。
- 边界：签名只证明“持有该主机密钥的一方报告了这些值”，不证明主机
  没有谎报自身信息，也不替代 `server-key` 与 TLS；签名在注册时验证，
  lookup 响应的完整性仍由 HTTPS 与服务器签名绑定保护。
- 文件：`server/internal/identity/identity.go`、
  `server/internal/registry/`、`server/internal/api/api.go`、
  `server/internal/agent/client.go`、`server/cmd/chast-server/main.go`、
  `server/internal/admin/admin.go`、
  `server/internal/webadmin/assets/{index.html,app.js}`。

### DEV-08 地址簿拼音/模糊搜索与持久化（P2）

- 目标：补齐搜索体验细节（非对齐缺口）。
- 实施：拼音首字母与子串模糊匹配；搜索词按视图持久化到
  `ui-filter.json`，清空即删除。
- 验收：中文/英文/拼音均可命中；“我的设备”与共享对话框互不覆盖；
  重启后恢复。
- 测试：Dart 单测与 widget 测试。

### DEV-09 更多采集/编码后端（P2）

- 目标：按硬件能力扩展 NVENC/QSV/AMF 或 Media Foundation 硬件编码。
- 实施：在 `src/codec/encoder_factory.cc` 增加运行时探测与回退；
  记录实际后端；保持 HEVC 录制 hvcC 路径一致。
- 验收：有对应硬件的机器优先硬件编码，失败回退软件；
  码率/延迟不劣于现有后端。
- 测试：工厂单测（无硬件走回退）；硬件机器回环实测。

### DEV-10 跨子网/中继协助 Wake-on-LAN（P2）

- 目标：超出 RustDesk 的增强，支持跨子网唤醒。
- 实施：服务器/同网段代理代发魔术包；鉴权与防滥用；可选审计。
- 验收：跨子网可唤醒；未授权请求被拒；与现有同网段直发共存。
- 说明：RustDesk 同样只支持同网段，本项不计入对齐缺口。

## C. 平台与驱动（PLAT）

### PLAT-01 虚拟显示器（P2）

- 目标：无物理显示器时提供虚拟桌面。
- 前置：EV 代码签名证书；IddCx 驱动开发与 WHQL/签名流程。
- 实施：驱动安装/卸载、分辨率枚举、与现有采集/输入映射对接。
- 验收：未接显示器的主机可远程操作；卸载后系统恢复；驱动签名有效。

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
2. **DEV-02、DEV-03**：DEV-01（更新链路）与 DEV-07（主机元数据签名）
   已完成，剩余安全边界关闭；可观测性与存储扩展为后续规模化铺路。
3. **DEV-04、DEV-05、DEV-06**：运营能力对齐。
4. **ENV-05~ENV-12**：按硬件/网络条件穿插执行，每项完成后在
   `docs/status-2026-10-25.md` 增补证据小节。
5. **DEV-08、DEV-09、DEV-10**：体验与增强项。
6. **PLAT-01~PLAT-03**：等签名证书与平台资源就绪后启动。

## 处置结论（2026-11-05）

`docs/code-review-2026-11-04.md` 的 C1–C5、H1–H7、M1–M4、M7–M19、M24–M29、
L1–L16 已修复并验证（C++ 目标构建通过；Go 与 Flutter 全量测试通过）。2026-11-05
M5/M6、M20–M23 亦已修复（文件撤权中止 outgoing upload 并逐块复查权限；发送队列
按代数复位并加超时；UPnP 应答源与 controlURL 校验、控制报文来源矩阵、对端地址
证明、TCP accept 退避重试）。复审确认 **M30 与原生查看器 V1–V13 仍未修复**，与
代码审核文档第 7 节第五批一致；本清单不把上述条目计入“已关闭”。本轮逐项复核
本清单：能力面没有新增代码级缺失，剩余项按“真实环境验收 / 本机开发 / 平台前置”
三类处置。全部条目保留编号，不删除，也不提前标记完成。

另：DEV-11～DEV-27 的实现晚于 2026-11-04 的代码审核，尚未做专项安全复审；
发布前应针对新增协议面（语音、终端持久化、远程配置、代理、OIDC、触摸、
WebSocket、便携模式、RDP）补一轮。

### 代码审核残余项（2026-11-05 复审）

M5、M6、M20–M23 已于 2026-11-05 修复（修复说明见
`docs/code-review-2026-11-04.md` 对应小节），下表只保留仍未修复的条目。

| 编号 | 位置 | 残余问题 |
| --- | --- | --- |
| M30 | 采集/解码/音频 | 光标 Pitch 与相机 stride/长度几何未校验；VPL 重试无上限；WASAPI 销毁与 `push()` 竞态 |
| V1/V3/V5/V6 | `app/windows/viewer/` | 相机输出锁外使用、`OpenTerminal` 状态不回滚、重复 `start` 覆盖输出、录制开关跨会话粘滞 |
| V2/V4/V7/V8 | `app/windows/viewer/` | WebAuthn busy 卡死、GPU 错误粘住无重建、目录遍历无上限、纹理尺寸无上限 |
| V9–V13 | `app/windows/viewer/` | 剪贴板路径、文件浏览路径、光标淘汰、适配器枚举、WebAuthn origin/rp_id 低危项 |

### ENV：保留为发布前验收

本机只有一台 Windows 工作站，没有第二台机器、公网出口、可管理路由器、
摄像头/HEVC 编码硬件、Windows Hello/FIDO2 与企业 CA，ENV-01～ENV-12 均无法
在本轮形成真机证据，状态保持“未验收”。下表的单机回环证据只能证明实现路径
存在，不能替代真实环境验收。

| ID | 状态 | 本地已有证据 | 仍需的环境 |
| --- | --- | --- | --- |
| ENV-01 | 未验收 | 打洞/中继/回退的单机回环（2026-10-26/27/31）；文件摘要与重连策略单测 | 两台机器、公网服务器、网络切换 |
| ENV-02 | 未验收 | v4/v6 会合、打洞、中继与共享载体回环（2026-10-26/27） | 真实 IPv6 出口与 AAAA |
| ENV-03 | 未验收 | 假 IGD 单测（SSDP/SOAP/718/714/续租/删除）与候选拼装 | 真实路由器、对称 NAT |
| ENV-04 | 未验收 | 弱网 A/B、Opus PLC、`starved_ms`、自适应/固定 lead 指标 | 双机 30 分钟可听验收，决定默认值 |
| ENV-05 | 未验收 | 端点跟随与 5 s 重建逻辑及单测；AEC 已实现 | 真实声卡热插拔、外放回声 |
| ENV-06 | 未验收 | 摄像头枚举/停止回环探针 | 真实摄像头媒体路径 |
| ENV-07 | 未验收 | FATE HEVC 码流封装/读回与 `quic_recording_hevc` | HEVC 编码硬件真实会话 |
| ENV-08 | 未验收 | `frameToDesktop` 的 7 项 Dart 测试（负 origin/双屏） | 双屏、不同缩放真机 |
| ENV-09 | 未验收 | 各功能单机回环 | 双机交互矩阵 |
| ENV-10 | 未验收 | WebAuthn 注册/断言/恢复码 API 与 Dart 测试 | Windows Hello/FIDO2 真实弹窗 |
| ENV-11 | 未验收 | 主窗、安全设置与连接服务对话框截图复核 | 审批窗、聊天窗、托盘英文布局截图 |
| ENV-12 | 未验收 | 临时 CA/mTLS/ACME 缓存回环、证书热加载 | 真实 CA、ACME、反代/CDN |

执行要求：ENV-04 的结论直接决定 `--adaptive-jitter` 是否默认开启，优先级
最高；ENV-05 可与 ENV-04 同一会话完成；ENV-01～ENV-03 建议同一次公网部署
完成；证据按“完成定义”归档到 `docs/status-2026-10-25.md`。

### DEV：按安全边界与依赖排序

| ID | 处置 | 说明 |
| --- | --- | --- |
| DEV-01 | 已完成（2026-11-05） | 发布者签名（服务器/主机/查看器三层校验）、`chast-publish` 签名工具、查看器健康标记回滚与安装器服务恢复均已实现并验证，详见同日小节。 |
| DEV-07 | 已完成（2026-11-05） | 主机用绑定私钥签名元数据，服务器验签后仅在规范化未改动时落库并回显；默认兼容旧主机，`--require-host-metadata-signature` 可强制；管理 CLI/Web 台显示签名状态，详见同日小节。 |
| DEV-02 | 采纳，P1 | 数据面（punch/relay/QUIC）与 SQLite/队列没有 span 或指标；排在 DEV-01/07 之后。 |
| DEV-03 | 采纳，P1 | 需 PostgreSQL 实例；先抽存储接口并保持 SQLite 默认部署与现有回归全绿。 |
| DEV-04 | 采纳，P1 | 账号仍只有逐设备标签，无按组授权与配额；与地址簿分组 ACL 解耦。 |
| DEV-05 | 采纳，P1 | GeoIP 目前只用于登录审计；连接/文件审计缺地点、路由与逐文件关联。 |
| DEV-06 | 采纳，P1 | 已有按日轮转与远端投递，缺集中去重与迟到合并。 |
| DEV-08 | 采纳，P2 小切片 | 拼音/模糊搜索与搜索词持久化属体验增强，可在 UI 批次完成。 |
| DEV-09 | 暂缓 | 需要 NVENC/QSV/AMF 等硬件；必须沿用零拷贝链路，新增后端只能直接写入现有 GPU 缓冲，不接受 CPU fallback。 |
| DEV-10 | 暂缓 | 超出 RustDesk 对齐面的可选增强，按实际需求排期。 |

### PLAT：维持暂缓

| ID | 处置 | 说明 |
| --- | --- | --- |
| PLAT-01 | 暂缓 | 缺 EV 代码签名证书与 IddCx/WHQL 流程。 |
| PLAT-02 | 暂缓 | 缺签名的 v4 打印机驱动与安装权限，没有免驱路径。 |
| PLAT-03 | 暂缓 | 需要 CI 与非 Windows 平台资源；先补 Linux 查看器再评估主机。 |

审核修复不改变上表能力面：M17–M19 让更新器在启动安装器前重新校验暂存
文件并把暂存目录移出用户可写路径，DEV-01 在此基础上补齐了发布者签名、
查看器健康回滚与安装器服务恢复；M24–M29 把用户态连接服务收紧为回环外
强制 HTTPS、密钥走 DPAPI、直连首用确认并限制终端 CSI/录制下载校验，不
改变 ENV-12 的服务端 CA/ACME/CDN 验收需求。

每项完成后更新上表状态、`roadmap.md` 与 `docs/status-2026-10-25.md` 并附
证据位置；ENV 项必须以真机日志/截图/指标关闭。

## 2026-11-05 RustDesk 1.5.1 复核（新增缺口）

复核基线：RustDesk `55786aa`（1.5.1，2026-11-03），含
`libs/base/protos/message.proto`。本轮把 RustDesk 当前源码与本项目逐项对照，
新增以下未出现在 ENV/DEV/PLAT-01～03 清单中的能力缺口，登记为
DEV-11～DEV-27。它们不是安全审核项，也不改变上一节 ENV/DEV/PLAT 的处置；
编号接在 DEV-10 之后，便于沿用“完成定义”归档。

| ID | 优先级 | 缺口 | RustDesk 证据 | chast 现状 |
| --- | --- | --- | --- | --- |
| DEV-11 | P1 | 多查看者并发（同一主机同时多个受控会话） | `src/server.rs:466`/`:488` 以连接表维护多个连接，视频服务按订阅者分发 | 已实现（2026-11-05）：`max_viewers`（1..8，默认 2）并发接纳，每个查看者独立会话/审批/权限/审计，共享桌面流与控制租约，详见同日小节 |
| DEV-12 | P1 | 出站 SOCKS5/HTTP(S) 代理 | `flutter/lib/desktop/pages/desktop_setting_page.dart:1938`（Socks5/Http(s) Proxy） | 已实现（2026-11-05）：会合控制面、TCP 载体与共享载体支持 http/https/socks5/socks5h，代理下强制 TCP 载体，详见同日小节 |
| DEV-13 | P2 | 语音通话（主机麦克风→查看器 + 呼叫协商） | `src/client/io_loop.rs:2540`、`src/server/connection.rs:4101`；`VoiceCallRequest/Response` | 已实现（2026-11-05）：协议 v37 新增语音呼叫协商与 `VoiceAudio`，主机按请求采集麦克风上行、查看器解码到输出端点，详见同日小节 |
| DEV-14 | P2 | 会话角色互换（Switch Sides） | `flutter/lib/common/widgets/toolbar.dart:702`、`src/client.rs:4564`/`:4913` | 不实施（2026-11-05 评估）：RustDesk 依赖对等端同时具备客户端与服务端；chast 查看器与主机栈分离，反向交接需合并两套栈或新增本机 IPC/一次性授权，超出单项范围，详见同日小节 |
| DEV-15 | P2 | 操作系统登录/自动登录 | `src/client.rs:3848`（`OSLogin`）、`:4459-4477`（自动登录）；`src/server/connection.rs:2856` | 已实现（2026-11-05）：协议 v39 的 `OsLoginRequest/Result` 由主机在登录或锁屏界面键入查看器提供的密码；权限位 0x2000 与 `allow_os_login` 默认关闭；密码不落盘、不记日志，详见同日小节 |
| DEV-16 | P2 | Windows 多会话选择（share-rdp 且存在多个会话时） | `src/server/connection.rs:2306-2322`（`WindowsSessions`）、`src/ui_session_interface.rs:1887-1897` | 已实现（2026-11-05）：协议 v40 的 `SessionListRequest/Result`、`SessionSelectRequest/Result` 由服务模式主机列出并切换 WTS 会话；权限位 0x4000 与 `allow_session_select` 默认关闭；查看器工具栏会话选择器在切换后自动重连，详见同日小节 |
| DEV-17 | P2 | 主机控制：远程重启、阻止本地输入、插入锁屏 | `toolbar.dart:661`/`:671`/`:683`；`src/server/connection.rs:691-697`、`:1255-1308` | 已实现（2026-11-05）：协议 v38 的 `HostActionRequest/Result` 支持重启、锁屏、阻止/恢复本地输入；权限位 0x0800/0x1000 默认关闭；会话结束或撤权自动解除阻止，详见同日小节 |
| DEV-18 | P2 | 独立会话类型：终端/摄像头/TCP 隧道无需桌面流 | `toolbar.dart:595-610` 分别发起文件、摄像头、终端、TCP tunneling 连接；`src/client.rs:3882` | 已实现（2026-11-05）：协议 v41 的 `SessionMode` 增加 Terminal/Camera/PortForward 与 `kServerInfoControlOnly`；主机为控制型会话单独派发 relay，不建立采集/编码；查看器对端菜单可直接发起三种独立会话，详见同日小节 |
| DEV-19 | P2 | 终端持久化服务（断线重连、服务 ID） | `src/server/terminal_service.rs:30-37`（100 个服务、1 小时空闲、1 MB 缓冲、重连回放）、`:105`/`:216` | 已实现（2026-11-05）：协议 v42 的 `TerminalRequest` 增加 persist/attach 标志与 `service_id`，主机进程级注册表保留最多 100 个服务，空闲 1 小时回收并回放 1 MiB 输出；`terminal_persist=` 默认关闭，详见同日小节 |
| DEV-20 | P2 | 多条端口转发规则并行 | `src/port_forward_mux.rs`、`flutter/lib/desktop/pages/port_forward_page.dart:294` 规则列表 | 已实现（2026-11-05）：查看器引擎维护规则表，每条规则独立 `forward_id`、本地监听端口、目标与计数，最多 8 条并行；面板列出规则并可逐条停止，详见同日小节 |
| DEV-21 | P2 | OIDC 账号登录 | `flutter/lib/common/widgets/login.dart:699`/`:732`、`src/hbbs_http/account.rs` | 已实现（2026-11-05）：服务端完成授权码 + PKCE 的 OIDC 登录（JWKS 验签、按 issuer 隔离的身份链接、可选自动建号），查看器用系统浏览器完成认证并轮询取回与密码登录相同的会话，详见同日小节 |
| DEV-22 | P2 | 触摸/笔输入 | `libs/base/protos/message.proto:190`（`PointerDeviceEvent`） | 已实现（2026-11-05）：协议 v43 的 `InputEvent` 增加 Touch/Pen，主机用 Windows 触摸注入与合成笔注入（不可用时降级为鼠标拖动），查看器映射多触点、压感、倾斜与橡皮/桶形键，详见同日小节 |
| DEV-23 | P3 | 编码扩展：I444/AV1/VP8/VP9 | `message.proto:22`（I444）、`:109`/`:117`、`:659-663`（VP9/H264/H265/VP8/AV1） | 已实现 AV1（2026-11-05，仅硬件零拷贝）：协议 v45 增加 AV1 wire 3，主机 oneVPL 的 AV1→HEVC→AVC 与 FFmpeg `av1_nvenc`/`av1_amf`/`av1_qsv` 阶梯，查看器用原生 `av1` 解码器走 D3D11VA；VP8/VP9 没有 Windows 硬件编码器，I444 需要新增 GPU 色彩通道与 4:4:4 硬件解码，受零拷贝约束不实现，详见同日小节 |
| DEV-24 | P2 | 多显示器查看体验与远程分辨率：独立窗口、跟随光标/窗口、缩放光标、修改主机显示分辨率 | `toolbar.dart:969-1040`、`:1157-1184`；`message.proto:598`/`:866`（`change_display_resolution`） | 已实现（2026-11-05）：协议 v44 的 `SetDisplayFollow/DisplayFollowState` 让主机跟随远程光标或前台窗口自动切换输出；远程分辨率与光标缩放此前已具备；RustDesk 1.5.1 的“按显示器独立开窗”只存配置、无消费方，暂不按多窗口架构实现，详见同日小节 |
| DEV-25 | P3 | WebSocket 传输 | `desktop_setting_page.dart:1945`（Use WebSocket） | 已实现（2026-11-05）：主机/中继的载体端口本就同时接受裸 TCP 与 WebSocket，本轮补上查看器“使用 WebSocket”开关（`server.json` 的 `use_websocket`），开启后引擎固定走 WS 载体，详见同日小节 |
| DEV-26 | P3 | 远程配置修改、会话结束后锁定 | `desktop_setting_page.dart:1286`（远程配置）、`libs/base/protos/message.proto:683`（`lock_after_session_end`） | 已实现（2026-11-05）：协议 v46 的 `RemoteConfigRequest/State` 在会话内读取并修改主机暴露的 14 项策略开关，权限位 0x8000 由 `allow_remote_config` 控制；`lock_after_session_end` 在会话收尾时锁定主机，详见同日小节 |
| DEV-27 | P3 | RDP 快捷方式与便携模式 | `flutter/lib/common/widgets/peer_card.dart:735-769`、`src/server/portable_service.rs` | 已实现（2026-11-05）：设备菜单新增 RDP 入口，经端口转发把查看器随机端口连到主机 127.0.0.1 并启动 mstsc（凭据由 mstsc 询问）；主机新增 `--portable`，设置/身份/审计留在程序目录，不写开机启动；RustDesk 式共享内存便携服务因违背零拷贝不实现，详见同日小节 |

### 排期建议

1. **DEV-11、DEV-12**：多查看者与出站代理属于部署/协作面 P1。DEV-11 改动
   `QuicSession` 单客户端假设、`HostServer` 单 `session_` 以及逐会话审批/
   权限/隐私/审计，工作量最大；DEV-12 独立且风险低，可先做。
2. **DEV-13～DEV-20**：远程支持常用能力，按“语音通话 → 主机控制 → 独立
   会话类型/持久终端 → 多端口转发 → OS 登录/Windows 会话 → Switch Sides”
   排序；其中 DEV-15/DEV-16 涉及凭据与会话边界，需先复审安全模型。
3. **DEV-21～DEV-27**：体验与部署增强。DEV-23 只在有对应硬件且能保持
   零拷贝时启动，不引入 CPU fallback；DEV-24/25 可随 UI 批次完成。

### DEV-11 多查看者并发会话（2026-11-05 完成）

同一主机可以同时服务多个查看者（`host.ini` 的 `max_viewers=1..8`，默认 2；
`1` 保留原有串行语义）。传输层 `QuicServerConfig::max_clients` 在 TLS 完成
前执行上限，超出的连接立即拒绝；每个通过上限的连接由 `accept()` 交给一个
独立的 `HostServer` 工作线程，各自拥有 Hello、审批、权限、流和审计记录，
共享安全策略、证书注册、监听器和 TCP 载体。

桌面视频按“发布者/跟随者”共享。DXGI 对同一输出只允许一个
`IDXOutputDuplication`，因此最先进入的桌面查看者成为发布者，负责采集、编码、
系统音频、光标和隐私遮罩，并把已编码访问单元、音频包、光标更新与桌面状态
广播给跟随者；广播使用 `send_frame_nowait()`，队列满时丢弃该查看者的这一帧，
慢查看者不会拖住采集循环。跟随者在自己的连接上运行控制面（输入、剪贴板、
聊天、文件、终端/端口转发/摄像头中继、麦克风、SAS），收到新的
`StreamInfo` 时用 `refresh_stream()` 更新描述。发布者离开后，一个跟随者赢得
`claim_desktop()` 并重建采集/编码/音频，以 `refresh_stream()` 接续已经宣告
的流；注册表版本号单调递增且不因接管重置，跟随者不会把旧描述当成新流。

输入仍只有一个控制租约：后来的桌面查看者以只读加入，租约在离开或输入被
撤销时释放。新增 `QuicSession::set_permission_ceiling()` 在流宣告后把
“后续可授予”的上限恢复到审批时的掩码（仍受配置硬门限裁剪），但不改变
当前掩码，因此只读查看者可以在租约释放后由操作员授予输入，而不会扩大
审批范围。隐私遮罩、显示切换/模式、流参数等主机级操作归发布者所有；跟随者
的请求以 Busy/Denied 明确拒绝，关键帧请求转发给发布者。

设置与审计：`HostSettings::max_viewers` 持久化到 `host.ini`，越界值回落
默认并告警；每个查看者按自己的 viewer ref 记录审计，重复引用由既有的
host-generated 回退 ID 区分。

验证：新增 `chast-host-multi-test.exe`（`ctest -R host_multi`，超时 180 s），
覆盖两个查看者并发、超限拒绝、重复审计引用、控制租约的授予/拒绝/释放与
发布者离开后的接管（含共享帧接收和租约恢复）。同轮
`chast-protocol-test`、`chast-host-settings-test`、`chast-carrier-test`、
`chast-host-tunnel-test` 通过；全量 `ctest` 37/42 通过，21 个 `quic_*`
端到端用例全部通过。剩余 5 项均为 `chast-viewer-probe.exe` 相关用例
（`viewer_folder_download`、`viewer_files_only`、`viewer_files_only_denied`、
`viewer_clipboard_files`、`viewer_clipboard_files_up`）在本机被 Smart App
Control / Code Integrity 以未签名二进制为由拒绝执行，状态 BAD_COMMAND，
属于已记录的本机环境限制，不是代码失败。

一次性运行（`run_seconds`>0 且 `serve_once`）的预算语义：listener 在首次
accept 时记录 `run_deadline`，把剩余秒数（下限 0.001，0 表示无限）传给
工作线程；工作线程到点自己结束会话，流和音频都经 `finish_server` 发 FIN。
listener 到期后只停止 accept，在 `finish_timeout_ms` 宽限后才强制停止，
跟随者循环同样按 `run_seconds` 退出。此前遗漏该预算会导致探测结束后主机
空转、音频无 FIN 被硬断（`audio errors=1`）。同轮修复
`chast-protocol-test` 中对空 `FileOffer` payload 截取导致的 UB 崩溃
（0xc0000409）：改为先构造合法 offer，再在线路上改写 size 字段验证拒绝。

边界：并发上限 8；同一时刻只有一个发布者，所有桌面查看者共享同一显示器与
编码器，因此跟随者不能单独切换显示器/分辨率/编码参数（请求会被告知由发布
者控制）；隐私遮罩仍是主机级状态。文件传输专用会话不参与桌面共享。媒体
路径仍沿用原有 GPU 零拷贝链路，未增加 CPU 转换或 CPU 编码 fallback；共享
只改变“已编码输出交给谁”，不改变采集/编码/渲染路径。

### DEV-12 出站 SOCKS5/HTTP(S) 代理（2026-11-05 完成）

查看器与主机的会合控制面、中继 TCP 载体和共享载体都可以走一个显式配置的
出站代理，支持 `http://`、`https://`、`socks5://`、`socks5h://`（必须带
端口，可带用户名密码）。实现分三层：

- Go 侧新增 `server/internal/netproxy`：手写 SOCKS5（RFC 1928/1929）与
  HTTP CONNECT 拨号（HTTPS 代理单独 TLS 握手），并提供 `chast-tunnel proxy`
  回环 HTTP 正向代理桥，供不支持 SOCKS5 的 Dart `HttpClient` 使用。
- 主机 agent：`--proxy`/`CHAST_PROXY` 覆盖控制面、更新下载和中继载体；
  `host.ini` 的 `proxy=` 与托盘对话框可持久化。查看器 agent 额外支持
  `--proxy-stdin`，代理 URL 不进入 argv。
- 查看器：`server.json` 的 `proxy` 在密钥或代理非空时整体走 DPAPI 保护，
  代理凭据不会明文落盘；服务器身份校验、账号/地址簿、录制上传下载全部经
  同一个出站路径；设置页新增“出站代理（可选）”与显示/隐藏按钮。

边界：UDP 直连、UDP 中继和打洞无法穿过 HTTP(S)/SOCKS5 代理（未实现
SOCKS5 UDP ASSOCIATE），因此配置代理后查看器强制走服务器 TCP 载体；
`--mode direct` 直接报错，服务端没有载体时也明确报错，不会静默绕过代理
直连。主机入站 UDP 监听不变；C++ 媒体链路未改动，零拷贝路径不受影响。
主机侧代理 URL 由 `host.ini` 读出后经 argv 传给 agent（无 stdin 通道），
带凭据时本机进程列表可见；查看器侧已经走 stdin。无效代理值一律失败关闭
（存储值保留并在使用时报错，不回落为直连）。主机名由代理侧解析。

验证：`flutter analyze --no-pub` 无问题；`flutter test --no-pub` 540 项
通过；`flutter build windows --release --no-pub` 成功；`go vet ./...`、
`go test ./...` 通过（新增 netproxy 单测与 host+viewer 代理回环测试）；
`chast-host-settings-test.exe`、`chast-host-tunnel-test.exe` 通过。

### DEV-13 语音通话（主机麦克风上行）（2026-11-05 完成）

协议从 v36 提升到 v37，在既有可靠控制流上新增三种消息：
`VoiceCallRequest(52)`（u8 标志 + u64 请求号，bit0 表示连接/断开）、
`VoiceCallResponse(53)`（请求号、Ok/Refused/Busy/Failed、原因字符串，
成功时附 Opus 采样率/声道/帧长与 ≤64 字节 extradata）和
`VoiceAudio(54)`（48 字节 `AudioHeader` + Opus 包，带长度与 CRC 校验）。
不新开 QUIC 流，也不占用 DATAGRAM 音频通道。

- 权限复用 `kPermissionMicrophone`。未授权立即 Refused，本连接已有通话
  Busy，采集启动失败 Failed；运行中撤销权限由两侧结束通话并下发停止。
- 主机每个 `HostServer` 工作线程独立服务语音（发布者与跟随者皆可），
  默认打开通信设备麦克风，`HostServerConfig::voice_capture` 可替换为
  测试采集器；逐包 `send_voice_audio`，发送失败计入 drops，关闭/撤权/
  采集失败都会停止并释放设备。统计新增 `voice_call_*` 计数。
- 查看器 `SetVoiceCall` 发起/结束通话，用响应参数创建 Opus 解码器与
  播放器（沿用已选输出端点、到达即播），工具栏新增通话按钮；状态发布为
  `voiceAuthorized/voiceEnabled/voicePending/voicePackets/voiceDropped/
  voiceErrors/voiceError`。

验证：`chast-host-voice-test`（合成采集器覆盖协商、包序、停止、取消后
重呼、撤权）通过；protocol/host_multi/quic_loopback 通过；`flutter
analyze` 无问题；`flutter test` 540 项通过；Windows release 构建成功；
全量 `ctest` 43/43 通过（644.27 s）。
边界：真实麦克风与音箱待双机验收；每连接一通通话；语音与系统声音共用
可靠控制流的拥塞控制；视频零拷贝链路未改动，无 CPU fallback。

### DEV-19 终端持久化服务（2026-11-05 完成）

协议从 v41 提升到 v42。`TerminalRequest` 在 id/尺寸之后增加 `flags`(u8) 与
`service_id`(u32)，载荷 13 字节：`kTerminalRequestPersist`(0x01) 要求主机
在会话结束后保留控制台，`kTerminalRequestAttach`(0x02) 按服务 ID 重连；
两者互斥，attach 必须带非零服务 ID，非 attach 必须为 0。`TerminalResult`
增加 `service_id`(u32)，只有 Ok 可以携带；新增
`TerminalCode::NotFound=5`。`ServerInfo` 增加
`kServerInfoTerminalPersist`(0x4000)，仅在授予终端权限时有效，announce/
refresh 时裁剪。

- **主机注册表**：`TerminalRelay` 进程级泄露单例保存最多
  `kMaxTerminalServices=100` 个共享控制台；会话级 `terminal_id` 用于 CTTY
  流，服务级 `service_id` 跨会话不变。attach 时改写为新会话的终端 ID，
  先把 1 MiB scrollback 复制进 replay 队列再送实时输出，避免重复或丢失；
  锁序固定为注册表 → 控制台。单会话仍最多 2 个控制台。
- **生命周期**：查看器下行 FIN 对持久控制台只解除绑定（detached），不关闭
  stdin；会话控制台保持原有 EOF 语义。分离期间读取线程继续把输出写入
  scrollback（1 MiB 环形裁剪）且不阻塞。60 秒扫描回收空闲 ≥1 小时、已退出、
  正在关闭或读端结束的服务；Job Object 以
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 保证主机进程退出时子进程一并终止。
  显式关闭（查看器 stop / shell 退出）仍立即移除服务。
- **主机设置与界面**：`host.ini` 新增 `terminal_persist=`，默认关闭；
  安全设置“远程终端”旁新增“断线后保留”复选框；关闭开关只影响新会话，
  已分离的服务保留到 attach 被拒或空闲超时（最长 1 小时）。
- **查看器**：状态新增 `terminal_persist_supported`/`terminal_service_id`；
  `OpenTerminal(columns, rows, service_id)` 自动选择 persist/attach；会话
  循环每次打开分配新的单调终端 ID，输入/缩放/停止/输出都使用该 ID；
  页面记住服务 ID，重连后重建面板并以 attach 打开；面板标题显示 `#id`，
  关闭按钮仍等价于结束服务，dispose 不再发送 close。
- **零拷贝**：仅改变终端控制/流生命周期，媒体采集、编码、解码与渲染路径
  未改动。

验证：`chast-protocol-test` 新增 v42 用例（persist/attach 往返、互斥与
service ID 一致性、Ok 携带服务 ID、错误结果禁止携带、HelloAck 保留持久化
标志）；`chast-host-settings-test` 覆盖 `terminal_persist=` 往返与默认值；
全量 C++ Release 构建成功。本机 Device Guard 阻止运行新生成的无签名
二进制（既有环境限制），C++ 侧为编译验证。`flutter analyze` 无问题；
`flutter test` 562 项通过；Windows release 构建成功。

边界：持久控制台的断线重连与空闲回收需双机验收；关闭 `terminal_persist`
不追溯已分离的服务；持久 shell 以主机账户继续运行，默认不开启。

### DEV-21 OIDC 账号登录（2026-11-05 完成）

服务端不引入新依赖，实现标准授权码 + PKCE 的 OIDC 联邦登录；查看器用系统
浏览器完成认证，令牌从不经过浏览器地址栏。

- **服务端 OIDC 客户端**（`internal/oidc`）：懒发现
  `issuer/.well-known/openid-configuration`（启动时不访问网络，IdP 不可用
  不阻塞启动），校验 issuer 回显与 https（仅回环允许 http）；授权 URL 带
  state、nonce、S256 PKCE；令牌交换用 Basic 认证并校验 ID Token
  （RS256/PS256/ES256 JWKS 验签、iss/aud/azp、exp/nbf/iat ±60s、nonce、
  sub）。
- **账号存储**：schema v6 新增 `oidc_identities(issuer, subject, user_id)`，
  subject 按 issuer 隔离；`LoginOIDC` 支持已链接账号登录与可选自动建号
  （随机不可用密码、按 preferred_username/邮箱本地部分/`oidc-<hash>` 派生
  用户名并在冲突时加 `-2…-50`、只接受已验证邮箱、首个账号仍为管理员），
  封禁账号拒绝；签发与密码登录相同的访问/刷新令牌。
- **HTTP 接口**：`GET /v1/account/oidc` 返回 `{enabled,name,auto_create}`
  （未配置也回 200 false）；`POST begin` 返回
  `auth_url/state/expires_in_ms`；浏览器回调
  `GET /v1/account/oidc/callback` 完成交换并把会话暂存在内存（10 分钟 TTL、
  512 上限、state 单次使用、`no-store`/`no-referrer`/CSP），渲染纯文本结果
  页；`POST poll` 以 202 表示等待、200 返回登录响应、401/404 带
  `account_banned`/`no_account`/`oidc_failed`/`oidc_expired` 错误码。令牌只
  经轮询返回，不出现在浏览器 URL、历史或 Referer 中。
- **服务端配置**：`--oidc-issuer/--oidc-client-id/--oidc-client-secret/
  --oidc-redirect-url/--oidc-scopes/--oidc-name/--oidc-auto-create`，issuer、
  client id、redirect 三项必须同时设置；client secret 可选（公共客户端 +
  PKCE），仅存内存、不写日志。
- **查看器**：`ServerApi.oidcInfo/oidcBegin/oidcPoll/loginOidc`；登录对话框
  探测到 SSO 后显示“使用单点登录”或“单点登录：<提供方>”，点击经原生
  `ShellExecuteW` 打开系统浏览器（仅 http/https），随后每 2 秒轮询直至
  成功、终止错误或 5 分钟超时；临时网络错误继续重试。新增
  `openExternalUrl` 桥接（C++ 侧校验长度与协议）。
- **零拷贝**：只新增账号登录控制面，采集、编码、解码与渲染路径未改动。

验证：`go build ./...`、`go vet ./...` 通过；`go test ./...` 全部通过
（新增 `internal/oidc` 提供方用例、`internal/account` 链接/建号用例与 API
端到端用例：begin→callback→poll 成功后令牌可用、state 单次使用、提供方
拒绝、未知 state、未配置时 503/false）；`flutter analyze` 无问题；
`flutter test` 574 项通过（新增 Dart 侧 info/begin/poll/完整登录/超时/
浏览器打开失败用例）；Windows release 构建成功（含原生
`openExternalUrl` 编译）。本机无真实 IdP，跨真实提供方登录与系统浏览器
交互需实机验收；C++ 桥接为编译验证（Device Guard 阻止运行新生成的无签名
二进制）。

边界：一个服务器只接一个 IdP；不实现 RP 发起的登出（会话仍由现有访问/
刷新令牌与封禁控制）；自动建号默认开启但只接受已验证邮箱；OIDC 登录绕过
本地 TOTP（强认证由 IdP 负责）；内存中的待处理登录在服务端重启后丢失。

### DEV-22 触摸/笔输入（2026-11-05 完成）

协议 v43 给 `InputEvent` 增加 Touch/Pen 两种输入，负载从 16 字节固定为
24 字节（在 kind/action/buttons/x/y/hid_usage/modifiers 之后追加
pressure/contact/flags/tilt_x/tilt_y）；旧的 16 字节事件一律拒绝，主机与
查看器必须同版本。

- **协议**（`include/chast/protocol.h`、`src/net/protocol.cc`）：
  `InputKind::Touch=3`/`Pen=4`；`TouchEvent{action,contact,x,y,pressure}`、
  `PenEvent{action,x,y,pressure,flags,tilt_x,tilt_y}`，动作共用
  `TouchAction{Down,Move,Up,Cancel}`，触点上限 `kMaxTouchContacts=10`。
  解析按 kind 校验：鼠标/键盘的扩展字段必须为零；触摸压力 0..1024、
  contact<10；笔 flags≤0x07、压力≤1024、倾斜 -90..90；多余字节拒绝。
- **主机注入**（`src/win/input.cc`）：`InitializeTouchInjection` 启用 10 点
  触摸，每帧携带全部活动触点（未变化的触点保持 INCONTACT），Up 沿用最后
  一次 Update 的坐标，取消发送 UP|CANCELED；合成笔用
  `CreateSyntheticPointerDevice(PT_PEN)` + `InjectSyntheticPointerInput`
  （Win10 1809+，运行时 GetProcAddress 加载，缺席不影响启动），坐标换算到
  虚拟屏左上角，倾斜直接映射为 Windows 每轴角度，压力/倾斜仅在非零时置位
  PEN_MASK，橡皮/翻转/桶形按钮映射 penFlags。注入不可用（旧系统、无触摸
  设备）时首个触点降级为左键拖动，其余触点忽略；`release_all` 取消全部
  触点与笔。注入仍支持 dry-run，测试不移动真实指针。
- **查看器**（`app/lib/remote_pointer.dart`、`widgets/remote_view.dart`、
  `pages/session_page.dart`、原生 `viewer_bridge.cc`）：触摸/手写笔指针
  不再落入鼠标路径；`RemoteContactTracker` 按指针分配并回收稳定触点槽，
  槽满时丢弃新触点；压力映射 0..1024（范围无效视为未上报），Flutter 的
  tilt/orientation 换算为 Windows 每轴角度，invertedStylus 映射翻转+橡皮，
  笔上主按钮映射桶形键；失去控制/焦点或重连时先逐触点 Cancel 再清空槽。
  原生桥按 kind 校验动作、槽位、压力、倾斜与互斥字段。
- **工具**：`tools/quic.cc` 的输入回放脚本加入双指触摸与带桶形键/倾斜的
  笔划，`same_event` 与 `--dump-input` 覆盖新字段；`--input` 验证仍使用
  dry-run 注入器。
- **零拷贝**：仅新增输入控制面，采集、编码、解码与渲染路径未改动。

验证：C++ Release 全量构建通过；`flutter analyze` 无问题；`flutter test`
579 项通过（新增触点槽分配/回收、压力与倾斜映射、笔标志用例）；Windows
release 构建成功。本机没有触摸屏/手写笔，且 Device Guard 阻止运行新生成
的无签名二进制，真实触摸/笔手势需实机验收。

边界：查看器只转发 Flutter 提供的 pointer 事件，不做手势识别；触摸 hover
不转发；多个手写笔同时书写时只跟踪第一支，第二支按下会替换当前笔。

### DEV-20 多条端口转发规则并行（2026-11-05 完成）

协议保持 v42：`PortForwardRequest/Result/Stop` 与 CPFW 流头一直按
`forward_id` 多路复用，主机侧 `kMaxPortForwardTargets=8` 也按目标表实现；
缺口只在查看器一次只维护一个活动目标。

- **查看器引擎**：`StartPortForward` 每次分配单调规则 ID 并返回（0 表示未
  入队），命令队列保持 start/stop 顺序；会话线程维护规则表，每条规则一个
  `PortForwardClient`、独立 `forward_id`、本地监听端口、目标与连接/失败/
  收发计数。规则按 `pump()` 驱动，最多 8 条并行；超出上限或绑定失败时保留
  一条带错误的规则供操作者查看和移除。主机撤权时一次性停止全部规则并在
  每条上写明原因。所有规则随会话结束销毁，监听端口不跨重连保留。
- **协议客户端**：`PortForwardClient::start` 接受显式 `forward_id`（拒绝 0），
  并暴露 `forward_id()`；命令行端口转发探针固定使用 1。
- **桥与界面**：`ViewerStatus` 用 `portForwardRules` 列表替代原单目标字段，
  `startPortForward` 返回规则 ID，`stopPortForward` 需要 `ruleId`。端口
  转发对话框改为规则管理面板：列出每条规则的 `127.0.0.1:本地 → 目标`、
  状态与计数，可逐条停止，并在下方添加新规则；独立端口转发会话的状态卡片
  同样列出规则并提供“添加规则”；工具栏的端口转发按钮在任一规则生效时
  高亮。
- **零拷贝**：只改变查看器侧的规则管理与控制面，媒体链路未改动。

验证：全量 C++ Release 构建成功（`PortForwardClient` 签名变更同步到 CLI
探针）。本机 Device Guard 阻止运行新生成的无签名二进制，C++ 侧为编译验证。
`flutter analyze` 无问题；`flutter test` 565 项通过（新增规则列表解析用例）；
Windows release 构建成功。

边界：真实 TCP 目标与多规则并发需双机验收；规则属于当前会话，断线重连后
需重新添加（与终端持久化不同）；同一目标可以出现在多条规则中，由操作者
自行避免重复。

### DEV-18 独立终端 / 摄像头 / TCP 隧道会话（2026-11-05 完成）

协议从 v40 提升到 v41：`SessionMode` 增加 `Terminal=2`、`Camera=3`、
`PortForward=4`；HelloAck/StreamInfo 在权限掩码之后追加一个 mode 字节；
新增标志 `kServerInfoControlOnly`（0x2000）表示会话没有桌面画面。解析器
强制 mode 与 files-only/control-only 一致，控制型会话不得携带画面、音频、
合并布局或显示器列表，权限掩码只能保留该模式对应的一个位。

- **主机策略**：`HostSecurity` 按 mode 把协商权限裁剪到单个能力位
  （Terminal→终端、Camera→摄像头、PortForward→端口转发）；未开启对应
  `allow_*` 时在握手阶段拒绝。Desktop 路径不变。
- **主机派发**：`HostServer::serve_current_session` 对三种控制型会话走新的
  `control_session`，`announce()` 不打开视频/音频流，不创建采集器或编码器；
  循环只泵 `TerminalRelay`/`CameraRelay`/`PortForwardRelay`。桌面专有请求
  （主机动作、OS 登录、会话列表/切换）一律回明确拒绝，避免查看器空等；
  控制型会话不参与多查看者 publisher/follower 注册。
- **查看器引擎**：`Start`/`Run` 增加 `session_mode`，状态新增
  `control_only` 与 `session_mode`；无画面会话跳过解码器、GPU sink 与帧
  停滞检查，HelloAck 后直接进入 streaming（摄像头仍走零拷贝 GPU 纹理）。
- **查看器界面**：设备卡片与最近连接菜单新增“远程终端”“主机摄像头”
  “端口转发”。独立会话页只有模式标题栏与对应面板：终端面板自动打开；
  摄像头面板自动注册纹理、列出设备并启动默认摄像头；端口转发弹出一次
  目标配置对话框，之后保留状态卡片（更换/停止）。
- **零拷贝**：控制型会话不触碰桌面采集/编码链路；摄像头复用既有
  `CameraRelay` 与 GPU 纹理路径，未引入 CPU fallback。

验证：`chast-protocol-test` 新增 v41 用例（三种 mode 的 Hello 往返、
HelloAck 的 mode/control_only 一致性、无画面/无媒体、权限裁剪、未知 mode
拒绝），全量 C++ Release 构建成功；本机 Device Guard 阻止运行新生成的无
签名二进制（既有环境限制），C++ 侧为编译验证。`flutter analyze` 无问题；
`flutter test` 558 项通过；Windows release 构建成功。

边界：终端 shell、真实摄像头与 TCP 目标待双机验收；独立摄像头会话每次
连接只服务一个查看者；桌面采集/编码链路未改动，仍保持零拷贝。

### DEV-17 远程重启 / 阻止本地输入 / 锁屏（2026-11-05 完成）

协议从 v37 提升到 v38，在可靠控制流上新增 `HostActionRequest(55)` 与
`HostActionResult(56)`：请求为 action(u8)+value(u8)，结果为 action(u8)+
当前阻止状态(u8)+结果码(u8)+原因字符串，结果码为
Ok/Denied/Unsupported/Busy/Failed。新增权限位 `kPermissionRestart`（0x0800）
与 `kPermissionBlockInput`（0x1000），默认关闭，随 HelloAck 权限掩码下发
（0x0800/0x1000 已被 FilesOnly/AudioDatagram 标志占用，因此不再复用标志位）。

- **主机闸门顺序**：会话权限 → 控制租约（非持有者回 Busy）→ 主机设置
  （`allow_restart`/`allow_block_input`，未开启回 Denied）→ 平台调用。
  锁屏沿用输入权限，未授权回 Denied。
- **平台实现**：重启 `ExitWindowsEx(EWX_REBOOT|EWX_FORCEIFHUNG)`（无管理员
  权限时回 Failed 并携带 Win32 错误）、锁屏 `LockWorkStation()`、阻止/恢复
  本地输入 `BlockInput(TRUE/FALSE)`；`HostServerConfig::host_control` 允许
  测试替换整个后端。
- **阻止状态生命周期**：`BlockInputGuard` 在会话退出时恢复本地输入；查看者
  主动解除、权限撤销、会话结束都会释放阻止，并向查看器补发一条 Denied 结果
  （原因 “the host released local input”）。统计新增
  `host_actions/host_action_failures/block_input_active`。
- **查看器**：工具栏新增电源菜单（锁定、阻止/恢复本地输入、重启），重启带
  二次确认；`RequestHostAction` 队列以最新请求为准；状态新增
  `restartSupported/blockInputSupported/blockInputActive/hostActionAction/
  hostActionCode/hostActionError/hostActionUpdates`。
- **托盘与设置**：安全对话框新增“允许远程重启”“允许阻止本地输入”复选框，
  托盘菜单可直接切换；`host.ini` 持久化为 `restart=`/`block_input=`；对已
  建立会话立即更新权限掩码，主机侧开关在下一次连接生效。

验证：新增 `chast-host-control-test`（真实 QUIC 回环 + 合成 `host_control`
后端）覆盖权限通告、锁屏/重启成功、后端失败上报原因与计数、阻止幂等与恢复、
撤权补发 Denied 且后端释放、查看器观察到权限变化、会话结束自动解禁；
protocol/host_settings/host_multi/host_voice/host_security/host_tunnel/
host_i18n 通过；`flutter analyze` 无问题、`flutter test` 547 项通过
（新增 host-action 状态与工具栏菜单用例）、Windows release 构建成功。
顺带修复语音通话状态在静态桌面下不写入 `status()` 的问题（接受/停止时立即
`publish_stats`，见 `docs/status-2026-10-25.md`）。

边界：真实重启需要主机进程具备管理员权限，测试注入后端避免重启本机；锁屏
使用 `LockWorkStation`，不模拟 Win+L 键入；`BlockInput` 只屏蔽本地物理
输入，不影响查看者注入；重启会中断会话；阻止状态在会话结束/进程退出时
释放。DEV-14（Switch Sides）仍因需要反向角色交接暂缓。

### DEV-15 操作系统登录 / 自动登录（2026-11-05 完成）

协议从 v38 提升到 v39，在可靠控制流上新增 `OsLoginRequest(57)`（原始
UTF-8 密码）与 `OsLoginResult(58)`（结果码 u8 + 原因字符串），结果码为
Ok/Denied/Unsupported/Busy/Failed。新增仅掩码权限位
`kPermissionOsLogin`（0x2000，没有对应的 HelloAck 标志位），默认关闭。

- **密码校验**：非空、≤256 字节、合法 UTF-8，且不含任何控制字符——注入器
  会把 `\n`/`\t` 转成回车/制表符，控制字符会提前拆散密码。结果原因截断到
  256 字节。
- **主机闸门顺序**：会话权限（input+oslogin 缺一即 Denied）→ 控制租约
  （非持有者回 Busy）→ 主机设置（`allow_os_login` 未开启回 Denied）→
  桌面状态（Unlocked/Unavailable 回 Unsupported）→ 注入。桌面状态在键入
  前即时复查：WTS 用户名为空或为 SYSTEM 视为登录界面；会话锁定标志 +
  `OpenInputDesktop` 得到非 “Default” 桌面视为锁屏；UAC 仍是已登录用户且
  未锁定 → Unlocked → 拒绝，确保密码不会键入普通桌面或 UAC 提示。
- **注入**：先 `InputInjector::release_held()` 清掉查看者按住的修饰键，再
  `submit_text(password + "\n")`；注入线程在每个排队项前重新跟随输入桌面，
  锁屏/登录界面切换后仍能键入。
- **凭据卫生**：密码只存在于请求队列与瞬时字符串；取出后、发送结果前用 0
  覆盖并清空，不进日志、不上命令行、不写盘；队列上限 4。查看器侧待发密码
  在停止或替换时清除，状态只回结果码与原因。
- **查看器**：工具栏新增密码按钮（需 input+osLogin），对话框只发送一次；
  状态新增 `osLoginSupported/osLoginCode/osLoginError/osLoginUpdates`；
  文件传输会话收到请求直接回 Unsupported。
- **托盘与设置**：安全对话框新增“系统登录”复选框、托盘菜单可直接切换，
  `host.ini` 持久化为 `os_login=`。与重启/阻止输入一致，主机侧开关对已
  建立会话可能需下一次连接才生效（会话权限上限在 open 时固定）。

验证：新增 `chast-host-login-test`（真实 QUIC 回环 + 合成桌面状态/注入
后端）覆盖权限通告、锁屏与登录界面成功、密码原样到达后端、Unlocked/
Unavailable 拒绝且不调用后端、注入失败原因与计数、本地拒绝空密码/换行、
撤权后客户端拒绝；`protocol_test` 新增 `OsLoginRequest/Result` 编解码与
边界用例；`host_settings_test` 覆盖 `os_login=` 往返与默认关闭；
`flutter analyze` 无问题、`flutter test` 552 项通过（新增
`os_login_status_test`）、Windows release 构建成功；全量 `ctest` 结果见
`docs/status-2026-10-25.md`。

边界：真实登录界面键入待双机/服务模式验收（测试用注入后端）；只有服务
模式（SYSTEM 会话）能打开 Winlogon，托盘主机在登录界面回 Unsupported；
不含“保存密码自动登录”，每次由查看器输入；媒体链路保持零拷贝，无 CPU
fallback。DEV-14（Switch Sides）仍因需要反向角色交接暂缓。

### DEV-16 Windows 多会话选择（2026-11-05 完成）

协议从 v39 提升到 v40，在可靠控制流上新增 `SessionListRequest(59)` /
`SessionListResult(60)` 与 `SessionSelectRequest(61)` / `SessionSelectResult(62)`。
列表结果携带结果码（Ok/Denied/Unsupported/Failed）、原因与至多 16 个
`RemoteSession{id,user,active,current}`；切换结果携带目标 id、结果码
（Ok/NotFound/Denied/Unsupported/Busy/Failed）与原因。新增仅掩码权限位
`kPermissionSessionSelect`（0x4000），默认关闭，`allow_session_select`
（`host.ini` 的 `session_select=`）是否开启都要求服务模式后端。

- **服务模式闸门**：worker 启动时用 `OpenEventW(EVENT_MODIFY_STATE,
  kWorkerSwitchEvent)` 探测服务切换通道；只有服务启动的 worker 才注册
  `list_sessions`/`select_session`，手工/托盘运行即使勾选设置也不会通告
  权限（日志说明原因）。列表与切换先查会话权限，再查主机设置，再查后端，
  切换额外要求控制租约（非持有者回 Busy）。
- **会话枚举**：`WTSEnumerateSessionsW` 跳过会话 0 与 0xFFFFFFFF，仅保留
  `WTSConnectState == WTSActive` 且有 `WTSUserName` 的会话；`current` 用
  `ProcessIdToSessionId(GetCurrentProcessId())` 标记。切换前重新枚举校验，
  目标必须仍然活跃且有用户，否则回 NotFound；选择当前会话是幂等的 Ok。
- **交接**：worker 将目标 id 写入 `%ProgramData%\chast\session-switch`
  （先写 `.tmp`，再 `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`），
  设置 `Local\chast-worker-switch` 事件；`SessionSelectResult(Ok)` 先入队，
  然后 worker 置停止标志。服务被唤醒后再次校验活跃+用户，停止旧 worker，
  用 winlogon 令牌在新会话重新启动 worker；旧会话的流随连接结束。
- **查看器**：工具栏新增“切换主机的 Windows 会话”按钮（需权限与服务端
  通告），对话框每 250 ms 刷新状态、显示用户/当前会话、支持手动刷新；
  切换被接受后页面等待会话结束，用既有重连阶梯（1/2/4/8/15 s）自动重连，
  落到新会话后随首帧重置。状态新增
  `sessionSelectSupported/sessions/sessionListCode/sessionListError/
  sessionListUpdates/sessionSelectId/sessionSelectCode/sessionSelectError/
  sessionSelectUpdates`。
- **结果卫生**：用户名字符串按 UTF-8 校验并限制 256 字节，列表最多 16 项、
  id 不得重复；原因截断到 256 字节。`make_session_select_result` 的缓冲区
  长度由单测覆盖（修复了少 2 字节导致的截断/越界写）。

验证：`protocol_test` 新增 `test_session_select`（列表/切换编解码往返、
非法 id、重复 id、未知码、非 Ok 携带条目、超长/畸形用户名、截断与尾部
字节）；`host_settings_test` 覆盖 `session_select=` 往返与默认关闭；
`flutter test` 新增 `session_select_status_test`（状态解析、非法 id 过滤、
`acceptsSessionSelect`、队列调用参数）。全量 `ctest`、`flutter analyze`、
Windows release 构建结果见 `docs/status-2026-10-25.md`。

边界：只有服务模式能切换；需要目标会话已登录且处于 Active；切换会结束
当前连接并由查看器重连；测试环境没有第二个已登录会话，WTS 枚举与真实
会话交接待双机/多会话验收；媒体链路保持零拷贝，无 CPU fallback。
DEV-14（Switch Sides）仍因需要反向角色交接暂缓。

### DEV-24 多显示器跟随与远程分辨率（2026-11-05 完成）

复核先把四项需求分开：修改主机显示分辨率与光标缩放此前已经具备——
主机侧 `ListDisplayModes/SetDisplayMode` 枚举并切换 DXGI 输出模式，查看器
分辨率菜单按流输出查询；光标由主机按 `_cursorScale = _viewScale × DPR`
随画面缩放绘制，无需新增开关。“按显示器独立开窗”在 RustDesk 1.5.1 中
`displays_as_individual_windows` 仅写入配置，没有消费方，产品面并未真正
提供，因此本轮不引入多窗口架构，只补齐真实缺口：跟随远程光标与跟随远程
前台窗口。

协议从 v43 提升到 v44，在可靠控制流上新增 `SetDisplayFollow(63)` /
`DisplayFollowState(64)`。请求负载为 1 字节掩码（bit0 光标、bit1 窗口，
未知位拒绝）；状态负载为掩码、结果码（Ok/Failed/Unsupported）、u16 长度
与 UTF-8 原因，结果码非 Ok 时掩码必须为 0，原因按字符边界截断到 256 字节。
传输层要求输入权限（与切换显示一致），服务端角色接收请求、客户端角色接收
状态。

- **主机跟随**：会话线程每 200 ms 采样一次。窗口跟随优先于光标跟随：
  `GetForegroundWindow` → `MonitorFromWindow` → 显示器工作区与 DXGI 输出
  矩形按交集面积取最大；光标跟随用 `GetCursorPos` 命中输出矩形。目标必须
  稳定 400 ms，距上次自动切换至少 1 s，并且仍受既有 1 s 切换冷却约束；
  这些都通过后直接调用既有 `switch_display`，重建采集/编码、发布
  `StreamInfo`、回 `DisplaySwitchResult` 的语义与手动切换完全一致。
- **拒绝路径**：合并流已同时显示全部输出，开启跟随回 `Unsupported`；无
  输入权限回 `Failed`；会话中途撤权时主机清除已生效掩码并主动发一条
  `Failed`（“input control was disabled”），查看器据此熄灭勾选。单显示器
  时主机仍接受请求但目标恒为当前输出，不产生切换。
- **查看器**：`SetDisplayFollow(cursor, window)` 进入单槽“最新覆盖”队列，
  状态新增 `displayFollowFlags/Code/Error/Results`。显示器菜单底部新增
  “跟随远程光标 / 跟随远程窗口”两个勾选项；少于两块屏、无输入权限或合并
  流时置灰。开启任一项会把本地选择切回“服务端默认”，让活动标记跟随
  `streamedDisplayIndex`；手动选择显示器前先关闭跟随，避免主机下一轮又把
  输出切走；拒绝原因以 SnackBar 呈现。

验证：`protocol_test` 新增 `test_display_follow`（两向编解码往返、未知
标志/未知结果码/尾随字节/超长原因拒绝、非 Ok 携带掩码拒绝、原因按字符
边界截断）；`flutter test` 新增跟随状态解析用例与显示器菜单用例（勾选
标记、点击回调、合并流置灰），全量 582 项通过；`flutter analyze` 无问题；
Windows release 构建通过。

边界：本机没有真实多显示器环境，光标/窗口命中所依赖的 DXGI 输出矩形与
虚拟桌面坐标只做了编译与单元验证，需双屏真机验收切换与去抖效果；跟随只
改变采集输出，媒体链路保持零拷贝，无 CPU fallback。

### DEV-23 AV1 硬件零拷贝编码（2026-11-05 完成）

按“只支持零拷贝、不考虑 CPU fallback”的方案先做可行性筛选：AV1 在
Windows 上有三家厂商的硬件编码器（oneVPL 2.x 的 AV1 VDEnc、NVENC
`av1_nvenc`、AMF `av1_amf`、QSV `av1_qsv`），可以沿用现有 D3D11 纹理
导入链路；VP8/VP9 没有 Windows 硬件编码器（只有 Linux VA-API 软件/硬件
封装），I444 需要新增 NV12→4:4:4 的 GPU 色彩通道，且 NVIDIA/AMD/Intel
的 D3D11VA 解码器都不向 FFmpeg 暴露 4:4:4 输出，两条路都无法在零拷贝
硬件路径内完成，因此本轮不实现，也不引入软件编码兜底。

协议从 v44 提升到 v45：`VideoCodec` 增加 `Av1`，帧头 codec wire 值 3，
`StreamCodecPreference::Av1 = 3`，`StreamOptionsResult` 的 codec 字段
同样接受 3；旧值 1/2 不变，未知值仍拒绝。

- **主机编码**：`host.ini` 的 `codec=av1` 与 `parse_codec_setting` 生效。
  自动模式沿用主机设置；查看器显式点选 AV1 时，若最终编码器不是 AV1，
  主机以 `Unsupported` 拒绝并写日志，与 HEVC 的显式语义一致。oneVPL
  阶梯为 AV1 VDENC → HEVC VDENC → AVC（AV1 没有 VME/legacy runtime
  实现）；FFmpeg 后端按厂商取 `av1_nvenc`/`av1_amf`/`av1_qsv`，构建中
  缺少该编码器、或当前 GPU 无法运行它（例如 Turing 驱动上的
  `av1_nvenc`）时，在 FFmpeg 后端内新建实例回退 HEVC→H.264。强制
  关键帧在 AV1 上只置 `MFX_FRAMETYPE_I`（AV1 没有 IDR 图片类型），
  FFmpeg 侧的关键帧判定依赖 `AV_PKT_FLAG_KEY`，`idr` 标志保持 0。
- **查看器解码**：`SetStreamOptions` 接受 3，状态里的 codec 字符串新增
  “av1”。FFmpeg 解码器对 AV1 固定选择原生 `av1` 解码器而不是
  `avcodec_find_decoder` 的默认项，因为 libdav1d 没有 AVHWAccel 注册；
  本机 FFmpeg 7 构建确认包含 `av1_d3d11va_hwaccel`，解码帧仍是 D3D11
  纹理，渲染链路不变。
- **查看器 UI**：`ViewerStreamCodec` 增加 `av1(3)`，编码器菜单新增 AV1
  项并标注“需要 AV1 硬件”；状态码 `av1` 回填到菜单与浮层标签；偏好
  持久化范围扩到 0..3。
- **录制与工具**：MP4 录制仍只支持 H.264/HEVC，AV1 会话启动录制会得到
  “MP4 recording supports H.264 and HEVC only”的明确拒绝，不产出坏文件；
  `chast-decode` 与 `chast-quic --in` 的 Annex B 回放同样对 AV1 给出明确
  错误。`chast-encode --codec av1` 可输出裸 OBU 流，QUIC 会话与摄像头
  中继直接以编码包传输，不需要 Annex B 拆分。

验证：C++ Release 全量构建通过；`protocol_test` 新增 AV1 帧头往返、
`SetStreamOptions`/`StreamOptionsResult` 的 AV1 往返（并把未知值测试改
到 4）；`host-settings-test` 覆盖 av1 解析与回写；`flutter analyze` 无
问题，`flutter test` 全量 582 项通过；Windows release 构建通过。

边界：本机没有 AV1 编码或解码硬件，AV1 路径只做了编译、协议与单元
验证，需在 Arc / Ada / RDNA3 等真机上验收画质、码率与 D3D11VA 解码；
媒体链路未改变，主机纹理直接进入硬件编码器、查看器解码到 D3D11 纹理，
全程保持零拷贝。

### DEV-25 WebSocket 强制传输（2026-11-05 完成）

载体的 WebSocket 服务端与自动回退此前已经具备：主机的 `CarrierServer`
在 QUIC 端口 + 1 上按首字节区分裸 TCP 与 WebSocket 升级，中继/共享载体
的 `UpgradeServer` 同样接受 `/chast` 的 RFC 6455 升级；查看器 `auto`
策略按 UDP → TCP → WebSocket 依次尝试，中继地址也在与 UDP 相同的端口上
提供 TCP/WS。真正的缺口是查看器没有 RustDesk 的“Use WebSocket”开关，
无法在只允许 WebSocket 的反代/CDN 后主动跳过 UDP 与裸 TCP。

- **设置**：`ServerSettings` 新增 `useWebSocket`，持久化到 `server.json`
  的 `use_websocket`（非机密，与 `use_recordings` 同层）；连接服务对话框
  新增“使用 WebSocket”开关，说明其用途是 WebSocket-only 反代或 CDN。
- **连接**：会话页在开关打开时向引擎传 `transport: 'ws'`，引擎固定只建
  一次 WebSocket 载体连接，不再尝试 UDP/TCP；载体内部仍是端到端 QUIC，
  证书固定、令牌、审批与权限语义不变，浮层显示“WebSocket 回退”。
  配置了出站代理时不强制 WS：此时隧道代理已把会话压到载体，引擎只有
  环回端点，开关不适用。
- **主机**：托盘与设置页的开关文案改为“弱网回退（TCP/WebSocket）”，
  监听行为不变，仍是一个端口同时服务两种载体。

验证：`flutter analyze` 无问题；`flutter test` 全量 583 项通过（新增
连接服务对话框开关用例、`use_websocket` 持久化往返）；Windows release
构建通过。C++ 只改了主机文案，载体实现未变，`carrier_test` 已覆盖
WS 握手、子协议、分片/超长消息拒绝与桥接往返。

边界：WebSocket 载体本身不带 TLS，加密由内层 QUIC 握手提供；若由反代/
CDN 终止 TLS，需要把 `/chast` 原样转发并保留 `chast-carrier.v1`
子协议。本机没有真实 CDN/反代环境，ENV-12 仍负责该场景的实机验收。

### DEV-26 远程配置修改与会话结束后锁定（2026-11-05 完成）

RustDesk 的“远程配置”允许已连接的查看者读取并修改主机暴露的策略开关，
`lock_after_session_end` 则在会话结束时锁定主机。chast 此前只有本机托盘
与安全页能改这些开关，查看器既读不到也改不了，也没有结束即锁定选项。

- **协议（v46）**：新增控制消息 `RemoteConfigRequest`(65) /
  `RemoteConfigState`(66)。请求携带 `keys` 与 `values` 两个 u32 位图：
  `keys = 0` 是读取，任一位表示写入；`values` 必须是 `keys` 的子集。
  应答携带结果码（Ok/Denied/Unsupported/Busy/Failed）、错误原因与仅 Ok
  才出现的开关快照；非 Ok 携带快照会被判为格式错误。共 14 项开关，
  `kRemoteConfigAll = 0x3FFF` 恰好用满 32 位中的 14 位。
- **权限**：新增 `kPermissionRemoteConfig = 0x8000`（最后一个空闲位；
  0x0040 属于 HelloAck 合并布局标记，仍保留不用）。主机仅在
  `allow_remote_config` 打开时把该位放入 HelloAck，查看器据此启停工具栏
  的“修改主机设置”按钮。
- **主机**：`handle_remote_config_requests` 校验位图、软件模拟开关与
  当前策略，写入沿用本机设置的同一条持久化路径；`allow_permission_change`
  打开时改动立即生效（含在线撤权），多查看者共享注册表快照。
  `lock_after_session_end` 在会话收尾时调用与托盘一致的锁定流程。
- **查看器**：引擎新增 `RequestRemoteConfig(keys, values)`（最新请求覆盖
  pending 槽）与状态字段（supported/flags/code/error/updates）；桥接与
  Dart 模型同步暴露；工具栏按钮打开设置对话框，先发一次读取，收到应答
  后展示 14 项开关，保存时只发送相对基线的差异位并等待主机 Ok 确认，
  被拒绝时保留编辑并显示原因。

验证：C++ Release 构建通过；`protocol_test` 新增 17 项位图/应答编解码
用例并通过；`host-settings-test` 覆盖新开关的往返与默认值并通过；
`chast-viewer-probe` 编译通过；`flutter analyze` 无问题；`flutter test`
全量 584 项通过（新增工具栏授权跟随用例）；Windows release 构建通过。

边界：本机单机验证，没有第二台真实主机做端到端远程配置验收；开关
语义仍以 `REMOTE_CONFIG` 暴露的范围为准，未暴露的策略（如安装、网络
适配器）不在远程修改之列。

### DEV-27 RDP 快捷方式与便携模式（2026-11-05 完成）

RustDesk 在设备菜单里直接提供 RDP：查看者经端口转发连到主机的 3389，
再用本机 mstsc 打开；便携版不安装，需要时临时拉起一个提权辅助进程共享
画面。chast 此前只能手工配置端口转发，主机也只能以安装后的服务形态运行。

- **RDP 快捷方式（查看器）**：设备卡片与最近连接的菜单新增“RDP”。选择
  后弹出端口对话框（默认 3389），随后用 `port_forward` 模式连接主机，
  会话转 live 时自动建立一条“本机随机端口 → 主机 `127.0.0.1:<RDP 端口>`”
  的转发规则，待引擎发布实际监听端口后启动
  `mstsc /v:127.0.0.1:<端口>`。规则和状态卡片沿用现有端口转发面板，
  关闭会话即断开转发。协议与主机不变：RDP 只是查看器侧的端口转发预设。
- **凭据**：与 RustDesk 不同，查看器不调用 `cmdkey`、不保存 RDP 用户名
  或密码，也不把密码放进任何命令行；mstsc 自己弹出登录框，凭据只留在
  Windows 的 RDP 客户端内。
- **便携模式（主机）**：`chast-host.exe --portable` 把设置写到
  `<程序目录>\chast-portable\host.ini`（权限、令牌、设备身份和审计
  spool 都随之落在该目录），复制文件夹即可带走完整身份；显式
  `--config` 仍然优先。便携模式强制关闭开机启动并禁用两个开关入口，
  避免把指向旧路径的登录项留给下一台机器。
- **不做 RustDesk 式“便携服务”**：它的做法是在提权辅助进程里采集，再用
  共享内存把帧复制给主进程，这与项目“只支持零拷贝、不考虑 CPU
  fallback”的约束冲突。便携模式直接以托盘进程跑完整采集→编码流水线，
  纹理仍直接进入硬件编码器；代价是登录取屏、安全桌面采集与 Windows
  会话切换这些依赖安装服务的功能在便携模式下不可用。

验证：`flutter analyze` 无问题；`flutter test` 全量 587 项通过（新增
RDP 参数用例、设备菜单端口对话框用例）；Windows release 构建通过；
主机侧 Release 构建通过，`chast-host-settings-test` 新增便携路径用例
并通过。边界：本机没有第二台可 RDP 的 Windows 主机，mstsc 端到端与
便携模式实际启动未运行验证；便携模式不改变媒体链路，保持零拷贝。

### DEV-14 会话角色互换（Switch Sides）（2026-11-05 评估：不实施）

RustDesk 的角色互换依赖“每个对等端同时是客户端和服务端”：受控端收到
`SwitchSidesRequest` 后登记一次性 UUID，拉起本机 GUI 查看器进程
（`--connect <控制端 id> --switch_uuid <uuid>`）并关闭原会话；控制端服务
用该 UUID 接受反向连接（`src/server/connection.rs:4001-4017`、
`src/client.rs:4564-4636`、`:4907-4919`）。切换时不需要重新采集或转码，
因为双方本来就具备采集/编码、解码/渲染、注入和监听能力。

chast 的两端是分离产品：查看器只有 QUIC 客户端、解码/渲染和输入发送，
没有采集/编码、输入注入或服务端；主机只有采集/编码、注入和服务端，没有
解码/渲染或查看器 UI。真正互换角色需要满足以下之一：

- 把两套栈合并进同一程序，让任一端都能受控或控制；或
- 由主机拉起查看器、由查看器拉起主机，并新增本机 IPC、一次性授权、
  控制端主机可用性检查与凭据交接。

第二条仍绕不开一次性授权：RustDesk 用 UUID 命令行参数完成，而本项目的
约束是不把凭据放上命令行或落盘；主机也无法在不经用户确认的情况下让
查看器变成受控端。两者都超出单个缺口项的范围，且本机没有第二台 Windows
主机，无法做端到端双机验收。

也评估过“反向 RDP 桥接”（主机经会话把 mstsc 接到查看器的 3389）：它只
适用于开启远程桌面的专业版/企业版 Windows，连接还会接管或注销查看器的
本地会话，与 RustDesk 的角色互换语义不等价，因此不采用。

结论：DEV-14 不实施，协议、主机和查看器均不改动，媒体链路与零拷贝约束
不受影响。若将来主机与查看器合并为单一对等程序，或引入可审计的一次性
反向授权协议，再重新评估。

### 已核对但不算缺口的项

- Ctrl+Alt+Del：已由 `SendSAS` 实现（`include/chast/sas.h`），不是缺口。
- 受信设备免二次验证：主机侧 `TrustStore` 已支持，查看器可勾选“信任此
  设备，下次跳过两步验证”（`app/lib/pages/session_page.dart:4494`），不是
  缺口。
- 剪贴板：已支持文本/HTML/RTF/DIB 与文件剪贴板（`protocol.h:408-452`），
  覆盖 RustDesk 的 `MultiClipboards`。
- 隐私模式、摄像头面板、终端面板、单目标端口转发、文件传输会话、查看器
  录制、地址簿分组/ACL、同网段 Wake-on-LAN、局域网发现、直连 IP、TLS/mTLS/
  ACME、2FA/WebAuthn/找回、管理台角色、审计导出/归档、i18n、安装器、
  IPv6/打洞/中继/共享载体、UPnP 均已实现，见 `docs/status-2026-10-25.md`。
- 白板（`src/whiteboard`）只在 RustDesk CLI/内部工具中出现，未进入 Flutter
  产品面，不计为对齐缺口；`PluginRequest` 属插件机制，也不是产品功能缺口。
- 虚拟显示器、远程打印、非 Windows/Web/移动客户端仍由 PLAT-01～03 跟踪，
  本表不重复登记。

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
