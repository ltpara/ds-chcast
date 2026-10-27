# RustDesk 服务端对照与计划修正（2026-10-26）

- 参照物：`D:\rustdesk` @ `c7f212d`，`libs/hbb_common` @ `5d52530`。
- 该检出只有客户端与公共库，**没有 hbbs/hbbr 源码**，因此对照基准取
  `libs/hbb_common/protos/rendezvous.proto`（协议契约）与客户端调用点
  （`src/rendezvous_mediator.rs`、`src/client.rs`、`src/common.rs`、
  `src/ui_interface.rs`、`src/lan.rs`）。
- 我们的实现以 `server/`（Go）为准，本文初稿写于协议 v25；端口转发（v26）与
  远程终端（v27）之后已落地，当前协议版本为 **v27**。

## 结论

没有根本性的架构偏差：注册/在线、打洞、中继、中继节点、限速与名单、软件
更新、批量在线、别名/标签等 hbbs 基线能力均已具备，账号、地址簿分组/共享/
条目级 OCC、审计、聊天、Wake-on-LAN 属于我们自研的增量。差异集中在两类：

1. **有意偏差**：控制协议格式、数据传输形态、权限模型、中继分配方式、
   授权与部署模型。这些是既定设计，不按 RustDesk 修改。
2. **真实缺口**：IPv6 双栈、查看器固定服务器公钥、主机 ID 迁移、UPnP、
   基于会合的 HTTP API 代理、局域网发现、审计关联 ID、账号令牌生命周期与
   登录地点。这些需要修正计划并补齐，其中前两项为 P0。截至 2026-10-26，
   IPv6 双栈、服务器公钥固定、主机 ID 迁移、审计关联 ID、令牌生命周期、
   UPnP、GeoIP 登录地点与局域网发现均已补齐（P1 全部完成，P2-2 完成）；
   端口转发原生链路与 CLI 回环验收完成（协议 v26，属主机/会话层，
   见 `docs/m11-p2-port-forward-plan.md`）；远程终端与查看器 Flutter 面板完成
   （协议 v27，见 `docs/m11-p2-terminal-plan.md`），端口转发 GUI 也已接入。
   剩余 HTTP 代理（低价值），以及所有跨网/双机验收。

## 1. 已对齐（hbbs 基线）

| 能力 | RustDesk 证据 | 我们的实现 |
| --- | --- | --- |
| 主机注册与保活 | `RegisterPk`（id/uuid/pk/no_register_device，`rendezvous.proto:113`）、`RegisterPkResponse.keep_alive`、`register_pk`（`rendezvous_mediator.rs:1244`） | `POST /v1/hosts` 注册 + `keepalive`/`keepalive_challenge`/`keepalive_auth` 挑战应答（`wire.go`） |
| 设备身份绑定 | `pk` + `uuid` 防止 ID 被顶替 | 服务器 Ed25519 签发主机绑定，代理可用 `--server-pubkey` 验证（`agent/host.go:275`） |
| 在线状态与批量查询 | `RegisterPeer`/`OnlineRequest` | `GET /v1/hosts`、`GET /v1/hosts?ids=`（≤200，`missing` 不整批失败） |
| 打洞 | `PunchHoleRequest`/`PunchHole`/`PunchHoleSent.socket_addr` | `punch_request`/`punch_peer`/观察地址回填，多候选并行探测 |
| NAT 类型 | `TestNatRequest`/`NatType.ASYMMETRIC/SYMMETRIC` | 代理自测并在注册与打洞答复中携带 `nat_type` |
| UPnP 端口 | `RendezvousMessage.upnp_port`（`rendezvous.proto:40,79,108,154,190`） | 可选 `upnp_port` 随保活/查询/打洞转发，`chast-tunnel --upnp` 自建映射 |
| 中继 | `RequestRelay`/`RelayResponse.relay_server`、每次连接一个 `uuid` | 端口租约 + `relay_hello/challenge/auth`、查看者令牌、`uuid` 防会话复用 |
| 中继节点 | hbbr | `chast-relay`：健康检查、租约、共享载体、带宽/黑白名单/降级 |
| 中继限速与名单 | hbbs/hbbr `single/total bandwidth`、`blacklist` | `internal/relay/policy.go`、管理台 `sb/tb/ba/br` |
| 软件更新 | `SoftwareUpdate` | `--software-version/url` → 注册/lookup/`/v1/server` 的 `software` |
| 别名/标签 | Pro 侧能力 | `PATCH /v1/identities/{id}`、管理台 `alias`/`tags` |
| 服务器地址轮换 | `rendezvous_servers`/`ConfigUpdate` | `rs` 轮换、粘性续约、`relay-servers` 列表 |

## 2. 有意偏差（保持，不照搬）

| 主题 | RustDesk | 我们 | 理由 |
| --- | --- | --- | --- |
| 控制协议 | protobuf `RendezvousMessage`，TCP/UDP 双通道 | `CHASTC1\n` + JSON，单 UDP | 自有协议，字段可读、易演进；TCP 只有中继与载体需要 |
| 数据传输 | WebRTC/ICE，必要时 TURN | QUIC（ALPN `chast/1`），中继转发 QUIC 数据报 | 既定传输选型 |
| 加密 | 服务器密钥交换 + `secure_tcp` | 可选 TLS + Ed25519 绑定 + QUIC TLS，`--server-key` Bearer | 不引入 RustDesk 的密钥交换方案 |
| 权限 | 服务器把 `ControlPermissions`/`ControlledContext` 转给被控端 | 权限在主机通道内端到端协商（v13+） | 服务器不参与权限判定 |
| 中继选择 | 客户端按 `relay_server`/节点选择，可配置 | 服务器按租约分配 + 轮换 + 独立节点回退 | 集中可控，便于审计与限速 |
| 授权/部署 | `licence_key`、Pro 特性与 ID 部署 | 无 licence，全部功能本地可用 | 产品模型不同 |
| 连接类型 | `conn_type`（文件/端口转发/RDP/终端）随打洞请求传递 | 不传递；能力由主机通道协商 | 避免服务器成为能力仲裁者 |

## 3. 真实缺口（按修正后的优先级）

### P0-1 双栈 / IPv6 会合与中继

- RustDesk：`socket_addr_v6` 出现在 `PunchHoleRequest`/`PunchHole`/
  `PunchHoleSent`/`PunchHoleResponse`/`RequestRelay`/`RelayResponse`/
  `FetchLocalAddr`/`LocalAddr`（`rendezvous.proto:41,80,109,155,190,203,214`），
  与 IPv4 地址并列传递。
- 我们：`wire.Message` 只有单个 `Candidate` 与 `LocalAddrs`（`server/internal/wire/wire.go`），
  服务器默认监听 `0.0.0.0`（`--punch-listen`、`--relay-listen`、`--http-listen`、
  `--carrier-listen`），一个 UDP socket 只能观测到客户端出站所选的那一族；
  代理侧虽有 IPv6 候选排序（`agent/util.go` 的 `rankAddr`/`familyAllowed`/
  `mergeCandidates`），但服务器不提供 IPv6 入口，双栈主机无法稳定建立 v6 直连。
- 影响：IPv6-only 或 CGNAT + IPv6 环境只能走中继；与 hbbs 的行为不一致。
- 状态（2026-10-26，会合、打洞与中继已完成）：`wire.Message` 增加
  `candidate_v6`/`local_addrs_v6`，registry 按族保存观测地址，注册与查询下发
  `punch_v6`/`candidate_v6`，服务器可用 `--punch-listen [::]` +
  `--advertise-v6` 提供双栈入口；主机与查看器同时向两个会合地址保活并按族
  打洞。单机双栈回环验收通过（`[::1]` 查看器 auto 模式直连 `[::1]:18434`
  并完成回显）。真实路径暴露并已修复：keepalive 挑战改为按地址族键控；主机
  控制报文来源过滤接受 `punch6`。
  中继部分：`relay.Manager` 改为“端口 + 按族监听”结构，`--relay-listen-v6`
  在同一端口号上开 IPv6 UDP/TCP，两族共用主机租约与端口统计，查看者槽位与
  限速会话按族独立；注册/查询的 `relays` 同时下发 v4 主入口与
  `--advertise-v6` 端点，主机向全部端点发 `relay_hello`，查看者按本地 socket
  族选端点，主机侧中继路径判定也改为匹配全部端点（此前只匹配主入口，v6
  中继会被误报为 `direct`）。回环验收：服务端
  `--relay-listen 127.0.0.1 --relay-listen-v6 ::1`、主机/查看者均绑定
  `[::1]`，查看者选中 `relay [::1]:41200`，经 IPv6 中继完成 H.264
  1920×1080 QUIC 会话（842 帧、0 CRC/解码错误、RTT 1.4 ms），主机侧事件为
  `path=relay`；主机绑定双栈 `[::]`、经 IPv4 注册/保活并转发到 IPv4 本地
  QUIC 端点的部署形态也通过（842 帧、RTT 1.9 ms）。剩余：显式绑定 `[::1]`
  的单族主机无法跨族访问 IPv4 会合与本地转发（应绑 `[::]`，README 已注明）、
  共享 TCP 载体仍只在 IPv4 主入口、双机跨网复核。

### P0-2 查看器固定服务器公钥

- RustDesk：内置 `RS_PUB_KEY`（`config.rs:118`），`get_key`/`KeyExchange`
  校验（`common.rs:2111`、`2157`），`get_key_confirmed` 记录确认状态。
- 我们：服务器已有身份签名与 `--server-pubkey` 固定，但只有两个 Go CLI 代理
  暴露该开关（`cmd/chast-tunnel/main.go:46,140`）；Flutter 查看器只保存
  `serverKey`（`app/lib/target.dart`），无法固定会合服务器公钥，也无法在
  公钥变化时告警。
- 影响：查看器信任链只覆盖主机 QUIC 指纹（TOFU），会合服务器仍可被中间人
  替换后下发伪造主机绑定。
- 状态（2026-10-26，已完成）：新增 `app/lib/server_identity.dart`，按归一化
  服务器 URL 固定 Ed25519 公钥（指纹为公钥 SHA-256 大写十六进制），记录存
  `%APPDATA%\chast\server_identities.json`，固定/轮换/清除写入追加式
  `server_identity_audit.jsonl`。地址簿在线轮询直接比对查询响应的
  `server_public_key`，固定后不匹配即报错；显式连接先读 `/v1/server` 展示
  完整指纹（首次 TOFU），公钥变化必须在对话框中确认才轮换，取消即拒绝连接；
  确认后的固定值经 `--server-pubkey` 交给查看器代理，查询与打洞再由代理校验。
  设置对话框可查看并清除固定。回环验收：固定值匹配时主机注册、查看器解析并
  auto 直连打洞成功（`path=direct 127.0.0.1:18454`）；错误固定被拒
  （`lookup came from an unexpected server identity`）；服务器换用
  `id-b.key` 后，固定旧身份的查看器被同一错误拒绝，固定旧身份的主机刷新注册
  也被拒（`server identity does not match the pinned --server-pubkey`）。
  单测覆盖指纹解析/格式化、TOFU→轮换→清除审计、查询门禁与会话页对话框。

### P0-3 分组级 OCC 与分组共享/ACL

- RustDesk：地址簿按标签/分组组织，共享以整本地址簿为单位，没有分组级授权
  粒度；服务端不承担地址簿同步。
- 我们（M11 前）：分组只影响展示；授权记录只有 `(owner, grantee)` 两列，
  共享即整本地址簿，读写权限覆盖全部条目。
- 状态（2026-10-26，已完成）：
  - OCC：分组的创建/改名/删除与条目一样返回 `revision`，客户端可带
    `if_revision` 提交，冲突返回 409；`DeleteGroupAt` 沿用墓碑语义。
  - ACL 数据模型：`address_book_grants` 主键扩展为
    `(owner_user_id, grantee_user_id, group_id)`，`group_id = ''` 表示整本；
    旧库在 `Open` 时整表重建迁移，老行自动归为整本授权。
  - 读取过滤：`SharedBooks` 按授权过滤条目与分组，并返回 `scopes`；
    同一分组取整本与分组授权中的最高权限。
  - 写入约束：读写/完全控制只作用于被授权分组内的条目；分组级授权不能
    管理授权；删除分组即撤销该分组的授权。
  - HTTP API：`POST /v1/addressbook/grants` 与 shared 对应路径接受可选
    `group_id`；`DELETE .../grants/{user}` 不带查询参数仍撤销整对共享，
    `?group_id=` 只撤销单一范围；条目新增接受 `group_id`。
  - 查看器：共享对话框显示授权范围，新增/撤销/改权限都可选择分组；
    被授权分组的条目才显示编辑/删除菜单，添加条目只能落到可写分组。
  - 验证：`go test ./internal/account/ ./internal/api/` 通过；Flutter
    `flutter analyze --no-pub` 无告警，`flutter test` 264 项全通过；
    单机回环 `account_sync_smoke.dart ... share` 44 项全过（10 项分组范围）。
    覆盖旧库迁移、越权读写、跨分组添加、整本+分组叠加、组级控制不能管理授权
    与分组删除撤权。

### P1-1 主机 ID 迁移（`old_id`）——已完成

- RustDesk：`RegisterPk.old_id`（`rendezvous.proto:117`），改 ID 时携带旧 ID
  与 `uuid` 证明归属（`ui_interface.rs:1496-1576`）。
- 我们（有意偏差，2026-10-26 完成）：注册请求新增 `old_id` 与
  `migrate_signature`。证明归属的不是设备 uuid，而是**旧 ID 上已绑定的
  Ed25519 主机密钥**对 `(old_id, new_id, timestamp)` 的签名
  （`identity.MigrateMessage`，域 `chast-id-migrate-v1`）。只有持有旧密钥
  的实体能生成该签名，强度高于可被复制的 uuid。
- 服务端语义：旧 ID 存在且密钥匹配时，持久记录**原地改 ID**，保留
  `created_at`/`alias`/`tags`/指纹，删除旧记录并释放旧租约与中继槽位；
  旧 ID 属于其他密钥 → 403；新 ID 属于其他密钥 → 409；旧 ID 不存在 →
  幂等成功（迁移重试/已迁移后再启动）。注册响应回显 `migrated_from`，
  代理以 `--old-id` 发起，`ready` 事件携带 `migrated_from`。
- 主机端：托盘与服务把 `tunnel_old_id` 持久化到 INI，改 ID 时自动填入旧
  ID，收到 `migrated_from` 后清空，避免泄漏旧记录。
- 验证：`identity`/`registry`/`api`/`agent` 单测（含越权、新 ID 被占、
  幂等重试、元数据保留），C++ `host_settings`/`host_tunnel`；回环实测
  `devbox → newbox`：旧 ID 404、新 ID 在线且 alias/tags 保留、
  `migrated_from=devbox`、服务端日志 `host id migrated`，无 `old_id` 的
  普通注册不受影响。

### P1-2 UPnP 端口映射（`upnp_port`）——已完成

- RustDesk 随打洞/中继消息携带 `upnp_port`（`rendezvous.proto:40,79,108,154,190`），
  客户端自行做映射，hbbs 只转发。
- 我们（2026-10-26 完成，行为对齐、实现自研）：`wire.Message` 增加可选
  `upnp_port`；主机在 `keepalive_auth` 上报并存入租约，`GET /v1/hosts/{id}`
  返回该端口；查看者与主机在 `punch_request`/`punch_peer` 中互传，双方把
  “观察 IP + 映射外部端口”加入探针目标，对称 NAT 一侧在网关放行时仍可直连。
- 客户端映射（`internal/upnp`）：SSDP 发现 IGD
  （`urn:schemas-upnp-org:device:InternetGatewayDevice:1`），优先
  WANIPConnection:1、其次 :2、WANPPPConnection:1/:2；UDP `AddPortMapping`
  内外同端口、租期 1800 s，每 15 min 续租，退出删除（714 视为成功）；718
  冲突顺序尝试内部端口后的 19 个端口。`chast-tunnel host|viewer --upnp`
  显式开启，失败只记警告并回退原有直连/中继路径。
- 边界：IPv4-only；服务端不信任该字段，只校验 0..65535，候选仍以 UDP 观察
  为准；`--upnp` 缺省关闭，避免默认在用户网关上开洞。本机无 IGD（发现返回
  `no UPnP Internet Gateway Device answered`），真实路由器映射与对称 NAT
  直连需部署复核；现有证据是假 IGD 协议单测、代理候选拼装单测与回环上的
  字段双向转发。

### P1-3 主机侧入站审计与审计关联 ID——已完成

- RustDesk：`ControlledContext.conn_audit_ref` 随打洞/中继下发，供被控端
  把会话与审计记录关联。
- 我们（有意偏差，2026-10-26 完成）：关联 ID 由**查看器**生成（16 字节
  CSPRNG，小写十六进制），随 Hello 新增字段 `audit_ref` 传给主机（协议
  v25，只增不改；旧版本仍精确匹配）。主机不依赖会合服务器下发，离线/直连
  也能关联。
- 主机侧（`chast-host`）：会话结束（`success = ok && error.empty()`，含
  拒绝/失败）与每个入站上传完成/失败时，把记录追加到设置项 `audit_spool`
  指向的 JSONL（默认 `<host.ini 同目录>/<stem>.audit.jsonl`）。文件行按主机
  视角逐文件记录；主机不知道账号身份，行内只含引用、时间、方向、文件名、
  大小、结果与说明。
- 转发：`chast-tunnel host --audit-spool FILE` 每 2 s 批量读取完整行，用
  主机已注册的 Ed25519 密钥对域分隔消息签名（`chast-host-audit-v1`），
  `POST /v1/hosts/{id}/audit`。只有服务端落库后才截断 spool，短时断网不丢；
  时间戳偏差超过 2 分钟拒绝；非法/未知行丢弃。
- 服务端合并：会话按 `audit_ref` 合并查看器与主机两侧（谁先到都可），
  `source` 为 `viewer`/`host`/`both`，时间窗取并集、成功取与；文件不合并
  行，查看器按轮询窗口汇总、主机逐文件各一行，均带 `host_id`/`audit_ref`/
  `source`。运营者可用 `GET /v1/audit/connections?host=<id>` 与
  `GET /v1/audit/file-transfers?host=<id>` 按主机过滤。
- 查看器：原生引擎每会话生成引用并经状态暴露，Flutter 在会话结束与文件
  事件上报时带上 `audit_ref`；不再依赖服务端下发。
- 验收（2026-10-26 回环，真实进程）：`chast-server` ↔ 主机代理 +
  `chast-quic serve --audit-spool` ↔ 查看器代理 + `connect --audit-ref`。
  主机行先落库（`source=host`，账号为 `-`），查看器以账号登录后上报同一
  引用，合并为同一行（`username=audituser`、`host_id=audithost`、
  `source=both`，时间窗与客户端 IP 来自查看器）；重复上报返回同一 id，
  不新增行。同侧重复上报不会把 `source` 抬成 `both`，由
  `TestConnectionReportsMergeByAuditRef` 覆盖。主机文件行按文件逐条写入，
  spool 格式与隧道参数由 C++/Go 单测覆盖；查看器 UI 的真实上传留待下次
  GUI 验收。

### P1-4 令牌生命周期与登录地点（完成）

- RustDesk：Pro 账号体系由官方服务器承担，客户端保存登录态并在本地维护
  地址簿/设备管理；登录地点由官方服务记录。其公开源码不含账号服务端实现，
  没有可照搬的令牌格式，只能参考“长期会话 + 短期凭据”的行为目标。
- 我们（有意偏差，2026-10-26 完成）：自建账号服务端采用两段式令牌。
  - 访问令牌 30 min（`AccessTokenTTL`），刷新令牌 720 h（`TokenTTL`，即会话
    上限）。登录与 `POST /v1/account/refresh` 返回同一结构：
    `token`/`expires_in_ms`/`refresh_token`/`refresh_expires_in_ms`。
  - 存储：`user_tokens` 每会话一行，保存会话过期与 `access_expires_at`；
    `refresh_tokens` 按哈希保存 `session_id`/`rotated_at`/`replaced_by`。访问
    令牌过期只拒绝请求，不删除会话行，否则刷新无法轮换——这是实现过程中
    根因修复的一处真实缺陷。
  - 轮换与重放：刷新即轮换访问与刷新两枚令牌；被轮换的刷新令牌再次出现视为
    泄漏，整个会话立即撤销（访问令牌同步失效）。因此刷新响应丢失时客户端
    需重新登录，这是安全的默认取舍。
  - 生命周期联动：登出、撤销会话/其他会话、改密、管理员重置密码都会删除对应
    刷新令牌；旧库 `access_expires_at=0` 的行保持原有长有效期，升级不需迁移。
  - 审计：新增 `session_refresh` 动作，记录 `rotated`、`replay; session
    revoked`、`invalid token`、`missing token`、`internal error`，附带客户端 IP。
  - 查看器：`AccountSession` 持久化刷新令牌与刷新过期时间；访问令牌不足 2 分钟
    自动刷新（1 分钟定时器），401 清空会话，网络错误保留稍后重试；旧
    `account.json` 仍可加载。
- 验证（2026-10-26 单机回环，真实 `chast-server` + `account_sync_smoke.dart`）：
  39 项检查全过，覆盖轮换后旧访问令牌 401、新令牌可读账号、重放被拒并撤销
  轮换后的会话；管理台审计可见 `session_refresh ok detail=rotated` 与两条失败
  记录。`internal/account` 另覆盖访问/会话两段过期、轮换、重放、登出/撤权/
  改密连带删除、旧行兼容；`internal/api` 覆盖刷新接口与审计；Flutter 270 项
  测试含自动续期与 401 登出。
- 登录地点（2026-10-26 完成）：RustDesk 的地点来自官方服务；我们没有在线查询，
  改为可选的离线表 `--geoip-file`（每行 `CIDR 地点`，最长前缀匹配，IPv4/IPv6
  与 IPv4-mapped 归一）。未配置时地点为空，只记录 IP，不强制依赖任何数据。
  地点写入账号安全审计 `location` 与登录会话 `login_ip`/`location`，管理台
  审计与查看器设备会话对话框展示。单测覆盖最长前缀、坏行/缺文件、落库与
  API 响应；边界是表质量由运维负责，来源地址仍受 `--proxy-header` 影响。


### P2-1 基于会合的 HTTP API 代理（`HttpProxyRequest`）

- RustDesk：`HttpProxyRequest`/`HttpProxyResponse`（`rendezvous.proto:262`），
  客户端经会合服务器 TCP 代理访问主机的 HTTP API（`common.rs:1413-1520`）。
- 我们直接暴露 HTTP API（可配 TLS）；在反向代理/内网部署下不缺能力，
  价值主要在“只开一个端口”，排 P2。

### P2-2 局域网发现（`PeerDiscovery`）——已完成

- RustDesk 由客户端广播 `PeerDiscovery` 做同网段发现与 Wake-on-LAN
  （`src/lan.rs`）：受控端监听 `RENDEZVOUS_PORT + 3`（21119），查看器广播
  ping，pong 带 id/hostname/username/platform 与 MAC；与 hbbs 无关。
- 我们（有意偏差，2026-10-26 完成）：模型相同（主机应答、查看器广播），
  格式与端口独立：
  - UDP 21129，magic `CHASTL1\n` + 单行 JSON 的 `ping`/`pong`，单包上限
    2048 字节；不复用 RustDesk 的 protobuf 与 21119。
  - 主机侧由 `chast-tunnel host` 启动，默认开启，`--lan-discovery=false`
    关闭、`--lan-port` 改端口；只回答合法 ID，做每来源令牌桶限速（突发 5、
    每秒补 1），字段裁剪并去除控制字符。
  - 查看器侧 `chast-tunnel discover [--json] [--port] [--timeout] [--id]
    [--target]` 不需要连接服务；目标含受限广播、各在线非回环 IPv4 网卡的
    定向广播与回环，发两次（间隔 400 ms），默认等 1.5 s。同一主机的多来源
    pong 按 ID 合并，`address` 优先非回环地址，`addresses` 保留全部。
  - 暴露面与 RustDesk 一致：发现本身不认证，只返回 ID/主机名/用户名/平台/
    版本/MAC，不含密钥、密码、令牌或中继分配，也不授权连接；连接仍走扫描/
    密码/审批与证书固定。托盘“允许防火墙”为 `chast-tunnel.exe` 增加
    21129/UDP 按程序入站规则，禁用隧道/卸载时删除。
- 验收（2026-10-26 单机真实进程）：`chast-server` + `chast-tunnel host
  --id lanhost` + `chast-tunnel discover` 在真实网卡 `192.168.8.20` 的广播
  上得到一条合并记录（`192.168.8.20:21129` 与 `127.0.0.1:21129` 同一 ID，
  主机名/用户名/平台/版本/MAC 正确）；`--lan-discovery=false` 返回 `[]`；
  `--id lanhost` 自过滤为 `[]`；21129 被占用时记录
  `LAN discovery unavailable` 但主机仍上线。`internal/lan` 单测覆盖应答、
  自 ID/非法 ID/坏包/超长包、限速、目标列表、合并与字段裁剪。
  Windows 上 `ReadFromUDP` 对超长数据报返回 `WSAEMSGSIZE`（Linux 则截断），
  已按“单包错误、继续服务”处理，否则一个坏包会终止应答循环。
- 边界：单机回环只有一张网卡，同网段双机、跨子网（发现不跨路由）与
  IPv6-only 局域网仍需实机复核。查看器首页“扫描”已接入同一命令
  （`app/lib/lan_discovery.dart`），勾选后写入地址簿（要求已配置连接服务）；
  widget 测试覆盖对话框与保存，真实进程验收覆盖解析、合并与自过滤，尚未做
  人工点击验收。

### 明确不做

- `webrtc_sdp_offer`/ICE/STUN/TURN（QUIC 设计）。
- `licence_key`、Pro 功能开关、ID 部署与 `NOT_DEPLOYED`。
- `conn_type` 在服务器侧转发与据此限流（权限/能力由主机通道决定）。

## 4. 修正后的计划（M11）

在 `docs/status-2026-10-25.md` 与 `docs/roadmap.md` 中同步：

- **M11 P0-1 双栈会合与中继**（先做）
  - 服务器 `--punch-listen`/`--relay-listen`/`--carrier-listen`/`--http-listen`
    接受 IPv6，默认行为保持 IPv4 兼容（`0.0.0.0` 单栈不变，新增 `[::]` 用法）。
  - 注册与保活分别记录 v4/v6 观测地址；`wire.Message` 增加
    `candidate_v6`/`local_addrs_v6`（只增不改），打洞答复同时带回两族。
  - 代理侧按族维护候选并并行探测，仍由既有排序决定首选路径。
  - 中继按族分配与绑定（`relay.Manager.BindHost` 已支持按地址重绑，
    需扩展到 v6 与 `--relay-listen` 多地址）。
  - 验收：回环 `::1`/`127.0.0.1` 双栈注册、双族候选、v6 直连与 v6 中继；
    IPv4-only 部署行为不变。
  - 状态（2026-10-26）：会合、打洞与中继阶段完成。`candidate_v6`/
    `local_addrs_v6`/`punch_v6` 已落地，保活挑战按 (host, purpose, family)
    分键，主机与查看器都从两族会合地址收控制包。中继新增
    `--relay-listen-v6`：同端口号上的按族监听、共用主机租约、按族查看者
    槽位与统计，`relays` 同时下发 v4 主入口与 v6 端点，主机全端点认证、
    查看者按 socket 族选择、主机按全部端点判定中继路径。单机双栈回环已验证
    双族注册、双族候选、`[::1]` 直连与 `[::1]` 中继上的完整 H.264 QUIC
    会话（842 帧、0 错误）。剩余：纯 IPv6 主机经 IPv4 会合的降级路径、
    共享 TCP 载体的 v6 入口、真实跨网 IPv6 双机复核。
- **M11 P0-2 查看器固定服务器公钥**
  - `Target`/`account_store` 之外新增服务器身份记录：首次连接展示指纹并
    TOFU 保存，允许显式固定/更换；不匹配时拒绝查询与打洞。
  - 与 `--server-pubkey` 语义一致；服务器身份变化要可见、可审计。
  - 状态（2026-10-26）：完成。固定记录与审计独立于 `TrustStore`；地址簿轮询
    比对查询响应、显式连接经对话框确认并把固定值传给代理；回环验收覆盖匹配、
    错误固定、服务器轮换身份三种路径，详见 §3 P0-2 状态。
- **M11 P0-3 分组级 OCC 与分组共享/ACL**（原 P0，保留）
  - 状态（2026-10-26）：完成。分组 OCC（`revision`/`if_revision`）与分组级
    共享/ACL 均已落地；授权主键扩为 `(owner, grantee, group_id)`，读取按范围
    过滤、写入限于授权分组，删除分组即撤权；API 与查看器同步，详见 §3 P0-3。
- **M11 P1**
  - ID 迁移：完成（2026-10-26），详见 §3 P1-1。
  - 主机入站审计与关联 ID：完成（2026-10-26）。关联 ID 由查看器生成并随
    Hello 传递（v25），主机写 spool、隧道代理签名上报，服务端按引用合并，
    详见 §3 P1-3。
  - UPnP 字段与客户端映射：完成（2026-10-26）。`upnp_port` 随保活、查询与
    打洞双向转发，`--upnp` 可选启用 IGD 映射并定期续租，详见 §3 P1-2。
  - 令牌生命周期：完成（2026-10-26），详见 §3 P1-4。
  - 登录地点/GeoIP：完成（2026-10-26）。可选 `--geoip-file` 离线表把登录 IP
    映射为地点，写入账号安全审计与设备会话并在管理台/查看器展示；缺省只记录
    IP，详见 §3 P1-4。
- **M11 P2**：局域网发现完成（2026-10-26，详见 §3 P2-2）；端口转发完成
  （2026-10-26，协议 v26，权限位 `0x0080`，主机 `allow_port_forward` 默认
  关闭，`chast-quic verify --port-forward` 真实 TCP echo 逐字节校验通过，
  负向探针与 Flutter 对话框已补；剩余双机/中继验收）；远程终端完成
  （2026-10-27，协议 v27，主机 ConPTY + 查看器原生通道 + Flutter 面板，
  `verify --terminal` 真实 cmd.exe 回显通过，`flutter test` 303 项与
  Release 构建通过；剩余双机/中继验收与真实 GUI 点击复核，详见
  `docs/m11-p2-terminal-plan.md`）；HTTP API 代理保留但价值低（我们直接
  暴露 HTTP/TLS API）。

## 5. 完成标准

- 每项都有单元测试 + 回环验收记录，写回本文档与 `server/README.md`。
- 协议字段只增不改；语义变化提升版本号（当前 v27）。
- 不引入 RustDesk 代码或依赖；仅参考行为与字段语义。

## 6. 2026-10-27 复核：服务端剩余差距

代码层面 hbbs/hbbr 基线已无缺失项，剩下的差距分三类：

1. **验证边界（最高优先）**：双机跨 NAT 打洞/中继、真实跨网 IPv6、真实路由器
   UPnP 续租与对称 NAT、多网卡/DNS 轮换、`--relay-servers` 多地址、共享 TCP
   载体在反代/CDN 之后、多节点负载与带外故障、真实链路带宽整形——目前全部只有
   单机回环证据。共享 TCP 载体只在 IPv4 主入口提供；纯 IPv6 主机经 IPv4 会合
   的降级路径未做。
2. **功能缺口**：无角色/RBAC（`User` 只有 `id`/`username`）、无按组授权与配额；
   管理台是回环 CLI（非回环地址直接拒绝），没有 Web 管理台；HTTP API 代理
   （`HttpProxyRequest` 等价）未做且价值低；软件更新只下发版本与 URL，不托管、
   不签名、不自动下载安装。
3. **运维与规模化**：审计/连接/文件传输列表无分页与导出，只按 10000 条截断，
   无时间保留策略；地址簿墓碑不清理；`/metrics` 覆盖中继与打洞，缺请求延迟
   直方图与 tracing；日志无轮转归档；TLS 仅证书文件，无 ACME 与 mTLS；
   SQLite 单连接（`SetMaxOpenConns(1)`），无 Postgres/水平扩展与版本化迁移
   工具；账号无持久封禁、无邮件/短信找回、无 WebAuthn。
