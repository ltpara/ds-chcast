# G02/G03 受管准入与会话授权设计（修订草案）

> **2026-11-14 PostgreSQL 状态**：账号/对等存储在 PostgreSQL 13.23/14.25 上的
> 集成、v17→v18 迁移、13→14 恢复和双实例冷启动已通过；PostgreSQL 14.25 上的
> ACL 真主机整链也已在本机回环通过。生产 PostgreSQL 部署、生产反向代理/中继与
> 双机 Flutter 仍未验收。
> 证据见 [PostgreSQL 13/14 真库验收](postgres-acceptance-2026-11-14.md)。

2026-11-13 验收更新：真实 `chast-server.exe`、真实 `chast-host.exe`、真实 `chast-tunnel host` 与真实会话客户端已完成受管 ACL 单机回环验收。账号封禁、设备禁用/恢复、组授权/移组、权限收窄与越权拒绝、kid 退役和同公钥新 kid 恢复均已通过；服务端重启后主机回执策略 v6、新票据和新会话恢复。2026-11-14 又在本机 PostgreSQL 14.25 真库上重跑同一组 ACL 整链并通过。本文下方 2026-11-09 状态中“真主机整链未做”的表述由 [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md) 取代。生产服务端、真实反向代理/中继、双机 Flutter 查看器、生产 PostgreSQL 部署与真正更换 Ed25519 私钥仍未验收，整体状态继续保持“部分落地”。

状态：**部分落地**。本文件修正审核 D01–D03，作为 G02/G03 编码与验收的共同依据。2026-11-09 已完成第 5 节步骤 1 的服务端存储与管理 API：账号 schema v10（设备策略、允许用户/组、策略版本、主机回执）；并完成 lookup 的凭据分类与设备准入检查：共享 `link_key`/管理 key 在 restricted 设备上被拒绝，账号 token 按 ACL/禁用状态准入，batch 用 `denied` 区分“无权限”与“不存在”。票据 v1 字节格式已冻结（第 6 节），Go 侧独立签发、严格解析、验签与有界重放缓存已有黄金向量测试；服务端已提供账号令牌换取票据的 `POST /v1/access-tickets`（启动参数 `--access-issuer/--access-kid/--access-key`）。`go test ./...` 与 `go vet ./...` 通过。应用协议已升到 v62：查看器在控制流上先发 `AuthBegin`，主机回 `AuthChallenge`（32 字节随机数），Hello 携带 `access_ticket` 与 `ticket_proof`（查看器 Ed25519 密钥对 host_id、主机证书指纹、双方随机数与票据摘要的签名），`HostSecurity` 验票后校验持有证明，且只在证明通过后才消费 `jti`；回环自动化 `chast-managed-access-test` 覆盖有效票据、缺票据与错客户端密钥三种准入结果。lookup 对账号调用并携带连接临时公钥（`client_public_key`）时直接返回受管设备的 60 秒票据；主机对票据会话按单调时钟执行到期断开，不再等待查看器的统计/结束消息。C++ 主机已读取共享策略文件，受管设备对所有新会话失败关闭（直连、打洞、relay 与 carrier 共用 `HostSecurity`，因此同时生效）；主机在共享标记匹配后写出确认文件，Go 代理据此用设备密钥签名上报应用回执。C++ 票据准入也已接入 `HostSecurity`：`host.ini` 的 `access_issuer/access_kid/access_public_key` 与 `tunnel_id` 组成固定项，受管设备只接受验签通过、版本与共享标记一致且未重放的票据，权限取票据 ∩ 权限上限 ∩ 本机默认 ∩ 查看器请求，缺票据/缺固定项/坏标记全部失败关闭（`chast-host-security-test` 覆盖黄金票据与否定路径）。lookup 票据签发由 Go 单测覆盖（`TestLookupAccessTicket`），主机截止断开由回环测试 `chast-host-ticket-lease-test` 覆盖；主机与 C++ 客户端库的续租已落地（同连接、同主体、同客户端公钥，新 `jti` 通过后才移动单调时钟截止并收窄权限）。查看器已落地 Ed25519 客户端身份（DPAPI 持久化）、启动时在线取票与 20 秒自动续租；续租收窄权限会立即下发并拒绝新操作，账号/设备被拒后无法取得新票，存量会话最迟在原票据 60 秒截止处结束（65 秒上限由租约保证）。主机已支持重叠信任多个签发 `kid`（`host.ini` 可重复 `access_key=<kid>:<base64url>`），因此可以先发布新公钥、再切换服务端签发、最后移除旧 pin；撤销推送与生产服务端 + 真实代理的 16/16 链路验收已完成；真主机进程与真实受管会话的单机回环也已通过。剩余未验收边界为生产反向代理/中继、双机 Flutter、生产 PostgreSQL 上的完整 ACL 整链与真正更换 Ed25519 私钥，不能把本文当作完整的受管准入能力。2026-11-10 落地独立短期传输凭证的 RFC 段（第 3 节）：`chast-transport-ticket-v1` 域分离凭证按 `transport:punch/relay/carrier` audience 签发，绑定主机、查看器临时 Ed25519 公钥、主体、策略版本与 60 秒截止；lookup 以 `transport_public_key` 申请，UDP/TCP 打洞节点用固定 issuer 公钥验签并校验查看器对 nonce/时间戳的持有证明，受管设备拒绝 link/server key HMAC 回退，策略新增/删除即时同步 rendezvous 内存状态（`transport_test.go`、`transport_punch_test.go`、`transport_tickets_test.go` 与 Go 代理受管直连回环覆盖）；relay 与 carrier 已切换到传输凭证（2026-11-10）：内置 relay 的每主机端口、共享 carrier（TCP/WS）与独立 `chast-relay` 都要求查看器出示 `transport:relay`/`transport:carrier` audience 的短期凭证，并用凭证内公钥验证对 nonce/时间戳（±30 秒）的签名证明；重放缓存以 ticket/nonce/timestamp 为键并绑定来源地址，同源重传允许、异源拒绝、容量满失败关闭。受管设备在注册表与独立节点租约中带 `managed` 标记，旧 carrier token 一律拒绝；独立节点由 hbbs 在每次续租刷新该标记（策略解析先于租约分配，策略变更后的首次续租即携带新值）；策略变更时还会对持有活动租约的节点异步 `POST /v1/leases/{id}/managed` 立即翻转标记（只改标记，不动密钥/端口/到期），无活动租约时由下次注册收敛。`chast-relay` 以 `--access-issuer/--access-kid/--access-key` 固定签发公钥。Go 代理在凭证剩余不足 30 秒时经 lookup 刷新，relay 每 20 秒以新时间戳重签、carrier 每次重连签新 nonce。自动化覆盖 relay/carrier 验签、错 audience、坏证明、跨源重放、受管拒绝旧 token 与受管三路径端到端回环（`relay/transport_test.go`、`relaynode/auth_test.go`、`agent/transport_punch_test.go`）；真实 relay 节点/跨网链路仍未人工验证。G04 集中策略对象/分配/回退与真实链路验收已完成（2026-11-10），见第 8 节。2026-11-10 落地管理应急访问（第 3 节）：`POST /v1/emergency-access` 仅接受管理身份（账号 admin 或部署管理 key），要求非空原因（≤512 字节），绕过设备允许名单但仍受 `permission_ceiling`、主机本地认证与 60 秒票据/续租约束；同一端点签发访问票据和三份传输凭证，审计动作为独立的 `emergency_access`，主体记为 `emergency:<actor>`。查看器隧道已可用登录账号 token 认证 lookup（`--account-token-stdin`，2026-11-10），受限设备不再被共享 link key 挡住；管理应急访问的查看器入口已接线（2026-11-11，见第 3、5 节）；直连/载体续签由 agent 的 20 秒凭据刷新循环覆盖，此前真实链路手工验收未保留日志，需重跑并另存证据；管理台因票据绑定查看器自持密钥而只提供审计快速筛选；普通 ACL 的真主机单机回环已通过，生产代理/中继、双机 Flutter 与应急授权真实链路仍需验收。

## 1. 信任与签发权

- 访问票据使用独立的服务端 Ed25519 签发私钥，主机只保存受信任公钥。禁止用部署级 `host_key`、`link_key` 或管理 key 做票据 HMAC；注册凭据不具有授权签发权。
- 每个部署固定 `issuer`；每把授权公钥带 `kid` 和用途。首次纳管由管理员通过受保护渠道固定 issuer、公钥及主机设备公钥指纹，不能信任未认证 lookup 返回的任意公钥。
- 可参考现有 `server/internal/identity/identity.go` 的签名封装，但不复用角色共享秘密，也不自动把所有服务身份签名都当作授权。
- 签名字节固定为域前缀 `chast-access-ticket-v1\0` 加版本化载荷的原始字节。验签使用收到的原始字节，禁止通过重新序列化 JSON 验签；拒绝重复字段、未知版本/算法、超长字段和不合法权限位。编码、大小上限与跨 Go/C++ 黄金向量应先于实现冻结。
- 载荷至少包括：`issuer / audience / kid / host_id / host_public_key_fingerprint / subject / client_public_key / permissions / policy_version / iat / nbf / exp / jti`。主机 ID 重用不能继承旧设备票据；主体使用稳定账号或审计中的机器主体 ID。
- 轮换需先发布并确认新公钥，再切换签发；旧公钥仅在有界重叠期内有效。紧急撤销某 `kid` 触发策略更新、禁止续租并终止相关租约。未知 `kid` 或无法获得受信任的轮换链时拒绝，不自动下载并信任。

## 2. 受管状态是主机执行的策略

区分未纳管和受管状态，不能把 `open/restricted` 与本地一个可选布尔值当成两个独立开关。

| 状态 | 主机行为 | 控制台显示 |
| --- | --- | --- |
| 未纳管 open | 保留 v61 的本地认证语义；不宣称受中央 ACL 保护 | 未纳管 |
| 纳管/策略更新待确认 | 服务端停止签发与目标策略冲突的新凭证；等待主机落盘与确认 | 待应用，不能显示已保护 |
| 受管 open | 所有入口仍需中央签发的短期凭证；服务端可给被允许的连接角色签发，不要求账号 ACL | 已应用版本 N |
| 受管 restricted | 只允许通过账号/组 ACL 或显式管理应急授权的主体；旧共享 link key 无准入权 | 已应用版本 N |
| 受管但失联/策略过期 | 拒绝新连接；存量会话仅存活至已有租约截止，不能回退为未纳管 open | 失联/授权过期 |

最小纳管流程必须在 **G02** 完成：设备身份认证 → 受信任策略和公钥安装 → 原子落盘 → 主机安装入口守门 → 回执（设备身份、策略版本、应用结果）→ 控制台标记已应用。主机失联时无法承诺首次纳管或新策略已在本地生效。

受管标记、issuer、公钥、策略版本持久化到机器配置，服务 worker 和共享主机读取同一权威状态。已纳管主机遇到配置缺失/损坏、版本回退、验签失败必须拒绝受管连接并报告；受管标记应独立于可重建的策略缓存保存，不能仅由“是否找到缓存文件”推断。重启后需在线确认当前策略和时钟，再接收新会话。管理解除纳管必须是独立、有权限、有审计和本地确认的操作。

本地配置不能放宽中央限制；远程配置 API 不能关闭纳管守门。设备管理员能够替换二进制/系统信任根的情形属于受管操作系统的信任边界，不能宣称本方案能防御失陷的主机管理员。

## 3. 准入、传输与权限合成

lookup 必须显式解析一种凭据类型；多种相互冲突的身份同时出现时拒绝，不能依赖“管理 key > link key > Bearer”的静默优先级。账号鉴权后检查账号状态、设备状态及允许用户/组。管理应急访问必须显式请求、记录主体和原因，仍获得短期票据并经过主机本地认证；不能把原始管理 key 传给主机或普通查看器。已实现（2026-11-10）：`POST /v1/emergency-access` 只接受账号 admin 或部署管理 key，原因必填且不超过 512 字节；它绕过设备允许名单但保留 `permission_ceiling`、主机本地认证和 60 秒票据/续租，返回访问票据与 punch/relay/carrier 三份传输凭证，审计动作独立为 `emergency_access`，主体为 `emergency:<actor>`（服务端管理 key 记为 `emergency:server-key`）。查看器入口已接线（2026-11-11）：会话页提供管理员应急访问对话框，管理员账号登录取得 token 与必填原因后，经 tunnel stdin（`--emergency-token-stdin`、`--emergency-reason-stdin`、公开的 `--emergency-client-key`）申请覆盖；tunnel 把 `ready` 与后续 `access_ticket` 事件中的票据交给原生查看器，Dart 跟随续租事件更新票据。`chast-tunnel` 的每次传输凭证刷新都会重新申请覆盖，因此审计约每 40 秒新增一条 `emergency_access` 记录；直连和 carrier 会话没有 relay 的周期认证，agent 在应急模式下另起 20 秒凭据刷新循环，同时续签访问票据与 punch/relay/carrier 三份传输凭证，续租的新票据经 `OnAccessTicket` 交给原生查看器（`TestEmergencyAccessRenewsTicketWithoutRelay` 覆盖无 relay 时新票据到期时间后移且主体不变）。此前记录的一次真实链路手工验收（restricted lookup 403、票据验签、relay 凭证续期与回显、非管理员 403）未保留日志，不计为可复核证据，需重跑并另存证据。可复核的自动化覆盖包括服务端签发/拒绝/审计（`TestEmergencyAccessIssuesShortLivedTickets`、`TestEmergencyAccessRequiresExplicitAdminRequest`）与查看器受限设备准入（`TestEmergencyAccessViewerReachesRestrictedDevice`）。管理台不能安全代签票据（票据绑定查看器自持的临时公钥），故只提供“应急授权”审计快速筛选；多操作者审计汇聚与应急授权真实链路重跑仍待验收；普通 ACL 的 `chast-host.exe` 受管会话单机回环见 [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md)，不外推到应急授权。

应用访问票据的 audience 为具体设备/会话用途，传输凭证使用不同域前缀和 audience。UDP/TCP 打洞、relay、TCP/WS carrier 使用独立的短期传输凭证，绑定设备身份、客户端公钥、权限用途、策略版本和截止时间；传输节点使用受信任公钥或在线鉴权验证。不能把账号 Bearer token 填进原来的 link-key HMAC 字段，也不能让传输票据替代应用准入。打洞与 relay 现在使用同一套重放语义（2026-11-11）：以 `ticket.ID + nonce + timestamp` 为键记录已验证证明，同一来源重发相同证明可用（保留 UDP 重传与 TCP 重试），换源重放拒绝；缓存到期清理，容量 8192，满时拒绝新证明而不淘汰有效条目。`TestTransportPunchReplayRejected` 与 `TestTCPPunchReplayRejected` 覆盖同源重发、换源重放和容量边界。

客户端为连接生成临时密钥，在认证通道中提供挑战签名，证明持有票据绑定的 `client_public_key`。挑战绑定主机身份、连接和双方随机数。`jti` 需要已用状态：首次消费后只可用于指定会话的续租，不能用于第二次握手；主机重启后旧会话失效，在线重新取票。服务端与主机均限制已用状态容量，至少覆盖票据有效窗口；重放缓存满时拒绝新准入，不能任意淘汰尚有效条目。

v62 的认证消息按第 7 节编码；所有连接入口与 desktop/file/camera 等会话模式统一进入 `HostSecurity` 守门，之后仍执行本地密码、审批、TOTP 等认证。受管主机拒绝不支持该流程的旧客户端；不得自动降级或改走直连绕过。

权限按交集计算：服务端票据上限 ∩ 当前设备策略 ∩ 主机本地策略 ∩ 本次用户同意。任何本地同意只能收紧。G03 将该上限接入桌面输入、文件及文件剪贴板双向操作、终端、端口转发、摄像头和其他敏感入口，并在撤权时停止正在运行的操作、释放输入与关联资源。

Dart 将账号 token 通过受控 stdin 交给 Go agent（`--account-token-stdin`），agent 用它认证 lookup 并持有传输凭证；应用票据由 Dart 侧账号 lookup 获取后交给原生握手。账号 token 不进入 argv/日志，也不写磁盘；短期票据不落盘。凭证过期的重连必须重新获取授权。已落地（2026-11-10）：只有登录账号与目标部署一致时才传 token；link key 只保留为未签发传输凭证部署的打洞回退，绝不与账号 token 混用为 lookup 身份。`TestRestrictedDeviceLookupWithAccountToken` 覆盖受限设备经账号 token 准入与传输主体绑定，`TestAccountLookupKeepsLegacyPunchFallback` 覆盖未纳管部署仍用 link key 打洞。

## 4. 有界撤销：最多 65 秒

本版承诺有界撤销，不承诺离线瞬时撤销：

- 应用票据、会话租约、受管策略缓存和传输租约的有效期均不超过 **60 秒**；允许的时钟误差上限为 **5 秒**。超出误差或无法建立可信服务端时间时拒绝新授权。
- 会话每 **20 秒**主动续租；服务端每次续租重新检查账号/设备禁用、当前组成员关系、ACL、策略版本及 `kid`，不能只验证旧签名。续租被明确拒绝时立即断开并释放资源；网络失败不能延长原截止时间。
- 账号/设备禁用、移组或策略收紧后，新的 lookup 与续租立即拒绝；已签发票据的重用和已建立会话都必须在变更后的 **65 秒内**失效。服务端推送撤销/策略版本可缩短延迟，但正确性不依赖通知必达。
- 主机以服务端时间校验 `iat/nbf/exp`，使用单调时钟维护已验证租约的剩余期限（不超过 60 秒）；系统时钟回拨、休眠恢复、重复提交同票据不得重置或延长截止时间。恢复后先重新检查截止时间/在线确认，再执行敏感操作。
- 新连接同时受票据和本地主机策略缓存的截止时间限制。收到更新版本立即失效旧版本票据并复核存量会话；无法取得最新策略时不得把版本比较省略。
- relay/punch/carrier 续租同样重新校验主体授权；应用层即使还能收到转发的数据，也在主机租约到期时拒绝操作并断开。传输续期不能延长应用权限。

实施应为这些时限建立可注入时钟的测试；若产品需要更短上限，应整体收紧票据、缓存和租约，不能只改 lookup 票据 TTL。

## 5. 编码分片与验收矩阵

1. schema v10（SQLite/Postgres 同步）及管理 API：设备/账号/组策略、版本、目标状态、应用回执、审计。
   - 已落地（2026-11-09）：`device_policies` / `device_policy_users` / `device_policy_groups` / `device_policy_receipts` 四表；`SetDevicePolicy` 在版本递增的同一事务内替换允许名单；`CheckDeviceAdmission` 覆盖未纳管、open、restricted、禁用账号与组匹配；回执必须由设备注册公钥签名并拒绝过期版本；管理 API 为 `GET/PUT/DELETE /v1/device-policies/{peer}`、`GET /v1/device-policies`、`POST /v1/device-policies/{peer}/receipt`，写操作仅 admin 并记审计。管理台新增“设备策略”页（仅 admin，`server/internal/webadmin/assets`）：覆盖模式、32 位权限上限、允许用户/分组与删除；回执列只有在签名回执版本等于当前策略且 applied=true 时才显示“已应用版本 N”，否则显示“待应用”或“应用失败”及详情（静态断言见 `webadmin_test.go`，并已在真实浏览器中验证新建/编辑）。
   - 边界：存储/API/控制台、主机失败关闭与回执闭环均有自动化覆盖；真实链路的受管准入尚未验收，回执只表示主机报告。注册响应已携带 `device_policy`（mode/permission_ceiling/policy_version）；Go 主机代理已解析该字段并经 `HostInfo.Policy` 与 `chast-tunnel` 的 `device_policy` 事件上报，且将策略原子写入与主机共享的 `<stem>.policy.json`（启动时载入、写失败下次注册重试、解除纳管写 `managed=false`；`device_policy_test.go` 覆盖透传、去重、清除与落盘）。C++ 只派生该路径并通过 `--policy-state` 传给 agent（`host_settings_test`、`host_tunnel_test` 覆盖）。主机 `HostSecurity::evaluate` 每次新会话读取该文件：`managed=true` 或文件损坏时以 `Declined` 拒绝（原因说明等待 v62 票据），`managed=false` 或文件缺失保持本机认证；`security_config` 从设置传入路径，`host_security_test` 覆盖缺失、显式未纳管、受管与损坏四种状态。应用回执闭环已实现（2026-11-09）：主机额外派生 `<stem>.policy-ack.json` 并以 `--policy-ack` 传给 agent；`ready`/`relay` 事件携带 `device_policy` 时，主机只在共享标记的 mode、版本与更新时间都与事件一致后原子写入确认文件（托盘与 service worker 两个宿主都覆盖，`host_settings_test`/`host_tunnel_test` 校验路径、解析与拒绝写入）；Go 代理在后续注册时读取该确认文件，要求版本、模式和 `updated_at_ms` 精确匹配（防止策略删除后同版本重建复用旧确认），用设备私钥签署 `HostDevicePolicyReceiptMessage` 并异步 POST `/v1/device-policies/{peer}/receipt`，失败在下次注册重试且不阻塞 keepalive（`device_policy_test.go` 覆盖确认匹配、旧版本、异模式、旧时间戳与损坏文件）。**边界：回执只证明主机进程已确认该策略并正在失败关闭，不代表票据准入已可用；尚无真实链路的手工验收。** 权限上限交集、票据准入与运行中撤销已在前述票据/事件链路落地（含 `policy` 推送后按新版本中止存量会话）。
2. 独立签发、公钥固定与轮换、严格解析、身份和挑战绑定、重放状态；跨语言黄金向量与错误输入测试。
   - 已落地（2026-11-09）：`server/internal/acl` 完成 Ed25519 独立签发、主机侧公钥固定接口、逐字节严格解析、issuer/audience/时间校验、有界 `ReplayGuard`，并用冻结的 v1 黄金向量和错误输入测试锁定格式（第 6 节）。服务端 `POST /v1/access-tickets` 已接入：只接受账号 token，票据绑定 `audience=device:<peer>`、设备注册指纹、稳定账号 ID、客户端公钥与策略版本，请求权限与策略上限取交集后签 60 秒票据，`jti` 进入重放缓存并记审计；未配置签发密钥时端点返回 503。测试覆盖无/共享凭据拒绝、未知主机、ACL 拒绝、上限交集和验签字段。C++ 校验库已实现（2026-11-09）：`include/chast/access_ticket.h`、`src/host/access_ticket.cc` 逐字节严格解析，按 `kid` 查固定公钥并对原始 envelope 做 Ed25519 验签，校验 issuer/audience/主机 ID/设备指纹与 ±5 秒时间窗口，`TicketReplayGuard` 容量满时拒绝新准入；`chast-access-ticket-test` 复现第 6 节冻结黄金向量，并覆盖篡改签名、截断/尾随字节、未知 `kid`、越界时间、错主机/错指纹与重放。主机准入已接入（2026-11-09）：`HostSecurity::evaluate` 对受管设备强制票据，固定项来自 `host.ini`（`access_issuer`、`access_kid`、`access_public_key`，公钥为 base64url Ed25519），设备 ID 取 `tunnel_id`；票据权限与共享标记的 `permission_ceiling`、本机默认权限和查看器请求逐位取交集，策略版本必须等于共享标记版本，同一 `jti` 不能二次入场，缺票据、缺固定项、标记损坏、票据过期/重放/错设备/错指纹/版本不符均以 `Declined` 拒绝且不泄露口令状态；`chast-host-security-test` 用冻结黄金票据覆盖允许与全部否定路径，`host_settings_test` 覆盖固定项校验与往返。指纹比较忽略 `SHA256:` 前缀、`:` 分隔符与十六进制大小写（64 位摘要等价），服务端在签发时把裸摘要规范为 `SHA256:<小写十六进制>`（`access_tickets_fingerprint_test.go`）。v62 协议与主机准入已落地（2026-11-10）：Hello 携带票据与客户端持有证明，主机用一次性挑战校验证明后才消费 `jti`；`managed_access_test` 的回环链路覆盖有效票据准入、缺票据拒绝与错客户端密钥拒绝。lookup 票据签发已落地（2026-11-10）：账号身份 + `client_public_key` 的 lookup 对受管设备直接返回 60 秒票据，权限与策略上限取交集，共享凭据与未受管设备不返回票据（`TestLookupAccessTicket` 覆盖无密钥、共享密钥、短密钥、未纳管、restricted 拒绝、受管签发、权限收窄与坏权限）。主机租约截止已落地（2026-11-10）：票据准入的会话在服务端 `exp` 处按单调时钟截止，到期立即以 `ticket_expired_` 结束并只给 500 ms 结束宽限（`host_ticket_lease` 回环测试覆盖 3 秒票据的准入与按期断开）。续租已落地（2026-11-10）：控制流新增 `TicketRenew`（81）/`TicketRenewResult`（82），查看器用首个挑战与 Hello nonce 对新票据摘要签名；主机要求策略版本一致、主体与会话一致、`client_public_key` 与会话一致、挑战非零，全部通过后才消费新 `jti` 并移动单调时钟截止，权限按票据 ∩ 上限 ∩ 本机默认收窄（只能减不能增），拒绝时回结果并以 500 ms 宽限结束会话（`test_ticket_renew` 覆盖往返与坏输入，`host_ticket_lease` 覆盖成功续租与异密钥拒绝）。真实链路：生产服务端 + 真实代理的 16/16 验收已通过；真主机进程 + 真实受管会话的单机回环也已通过；生产代理/中继与双机组合仍待验收。撤销推送与在线终止已落地（2026-11-10）：schema v12 增加 `revoked_access_kids` 持久表，`GET/POST /v1/access-kids` 退役签发 `kid`（幂等、admin-only、记审计），签发端点发现当前签发 `kid` 已退役立即 503；`GET /v1/hosts/{id}/events` 以共享主机凭据 + 设备密钥对 `(hostID, after, timestamp)` 的 Ed25519 签名做长轮询，按主机推送 64 条环形缓冲内的 `policy`/`revoke`；Go 代理把策略写入共享文件并向主机 stdout 转 `policy`/`revoke`；C++ 主机只对票据准入会话按“策略版本落后”或“`kid` 匹配”断开（续租刷新匹配元数据），无关语句不影响会话。正确性仍以 60 秒票据 + 20 秒续租的 65 秒上限为界，推送只缩短延迟（`host_events_test.go`、`host_events_test`（agent）、`host_ticket_lease` 覆盖）。
3. 最小纳管下发/持久化/确认/失联拒绝；将策略守门接入各主机运行方式。
   - 已开始（2026-11-09）：服务端每次主机注册都在响应中下发 `device_policy`，读取策略失败时以 503 拒绝续租，避免受管主机误判为未纳管；测试覆盖受管、未纳管及删除策略后的注册响应。Go 代理已解析 `device_policy` 并经 `HostInfo.Policy`/`chast-tunnel` 事件上报，同时原子落盘到与主机共享的 `<stem>.policy.json`（原子替换、启动种子、失败重试、解除纳管保留 `managed=false` 标记）；C++ 派生路径、传 `--policy-state`，并在每个 Hello 读取该文件对受管设备失败关闭（含权限上限在内的其余执行细节留待 v62）。应用回执闭环已落地：C++ 在标记匹配时写 `<stem>.policy-ack.json` 并由 `--policy-ack` 告诉 agent，agent 用设备密钥签名异步上报，控制台按当前版本回执显示“已应用版本”。权限上限交集已在 Hello 准入时完成（票据权限 ∩ `permission_ceiling` ∩ 本机默认 ∩ 查看器请求）；v62 票据携带与持有证明已落地；票据会话的 60 秒截止已由主机单调时钟执行；主机已能按 `TicketRenew` 接受新票据并移动截止（同主体、同客户端公钥，权限只减不增）；查看器已在启动时取票并按 20 秒自动续租，续租收窄权限会立即下发，查看器随后发起的新文件操作被主机拒绝；账号/设备被拒后无法取得新票，存量会话最迟在原票据 60 秒截止处结束（65 秒上限由租约保证）；撤销推送已落地：`policy`/`revoke` 事件经主机长轮询下发，C++ 主机立即结束匹配的票据会话。
4. 🔶 账号 lookup 和独立传输凭证已打通 Go→Dart→原生（2026-11-10）；管理应急授权独立审计已在服务端落地（2026-11-10）：`POST /v1/emergency-access` 仅接受管理身份、原因必填且 ≤512 字节，绕过允许名单但保留权限上限与主机本地认证，签发访问票据与三份传输凭证，审计动作为 `emergency_access`、主体 `emergency:<actor>`；`emergency_access_test.go` 覆盖签发、验签、权限上限、管理 key 主体和全部拒绝路径。查看器隧道已改为用登录账号 token 认证 lookup（受限设备可准入，2026-11-10）；查看器应急入口、票据续租事件与管理台审计筛选已接线（2026-11-11）。此前一次真实链路手工验收（restricted lookup 403、票据验签、relay 凭证续期与回显、非管理员 403）未保留日志，不计为可复核证据，需重跑并另存证据；可复核的自动化覆盖包括服务端签发/拒绝/审计（`TestEmergencyAccessIssuesShortLivedTickets`、`TestEmergencyAccessRequiresExplicitAdminRequest`）、查看器受限设备准入（`TestEmergencyAccessViewerReachesRestrictedDevice`）与无 relay 时 20 秒票据/凭证续签（`TestEmergencyAccessRenewsTicketWithoutRelay`）。剩余为多操作者审计汇聚与真实应急链路重跑；普通受管 ACL 的真主机单机回环证据见 [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md)，不外推到应急授权。
5. 应用准入已在 v62 落地并通过回环自动化（票据 + 持有证明 + 挑战）；60 秒截止与主机侧续租已执行并自动化（`host_ticket_lease` 的到期、续租、拒绝与收窄四阶段），查看器已在启动时取票并按 20 秒自动续租；续租收窄权限会立即生效（第四阶段验证文件列表被主机拒绝）；65 秒撤销上限由票据租约与拒绝签发保证。G03 的源码侧权限交集与运行中操作撤权已覆盖：所有敏感请求在主机 handler 再校验会话掩码，而非只依赖查看器按钮；阻止本地输入、隐私模式、剪贴板、文件、麦克风、摄像头、终端、端口转发的撤权都会中止或释放对应资源，阻止输入、剪贴板、文件、摄像头、终端与端口转发已有自动化证据（终端/转发见第 7 节 2026-11-10 条）。真实 `chast-host.exe`、代理和视频会话的单机回环已通过；剩余生产反向代理/中继、双机 Flutter、生产 PostgreSQL 上的完整 ACL 整链与真正更换私钥。撤销推送已完成：服务端按主机长轮询下发 `policy`/`revoke`，Go 代理转 stdout 事件，C++ 主机按新策略版本或匹配签发 `kid` 立即断开票据准入会话（详见第 2 条）。
6. 扩展 G04：批量策略、多组优先级、组织范围与运维界面，沿用 G02 已建立的同步确认和拒绝回退机制。

必须把“新 lookup”“先取票再禁用/移组并重用”“已连接后撤权”分别乘以直连、UDP/TCP 打洞、relay、TCP/WS carrier 验证，并涵盖：

- 正常账号、无票据、过期、错主机/部署、主机 ID 重用、错权限、错客户端密钥、重放、未知/撤销 kid、密钥轮换。
- 注册主机持有 `host_key` 仍不能伪造任何访问票据；普通 link-key 在 restricted 下无准入权。
- 服务失联、撤销通知丢失、乱序策略、受管配置损坏/回退、主机重启、休眠恢复、时钟前后跳、旧客户端。
- 管理应急授权仍受短租约、本地认证和审计约束；未纳管旧行为与受管 open 的差异明确显示。服务端自动化已覆盖签发、拒绝与审计（`TestEmergencyAccessIssuesShortLivedTickets`、`TestEmergencyAccessRequiresExplicitAdminRequest`）；查看器入口已接线，直连/载体续签由 `TestEmergencyAccessRenewsTicketWithoutRelay` 覆盖；普通 ACL 的真主机单机回环已通过，但应急授权的真实链路手工验收未保留日志，仍需重跑并另存证据；管理台只提供审计快速筛选，生产代理/中继与双机整链仍待验收。
- 收紧敏感权限后，已开启的终端/转发/文件流等在规定时限内停止，不能仅禁用 Flutter 按钮。

完成 G02/G03 必须分别提交当前源码自动化和真实链路证据；本设计修订本身不算 ACL 功能完成。

## 6. 访问票据字节格式 v1（2026-11-09 冻结）

实现：`server/internal/acl/ticket.go`、`server/internal/acl/replay.go`；C++ 校验实现：`include/chast/access_ticket.h`、`src/host/access_ticket.cc`；黄金向量：`server/internal/acl/ticket_test.go` 的 `TestGoldenTicket` 与 `tests/access_ticket_test.cc`。C++ 端按本节逐字节实现；验签使用收到的原始 envelope，禁止重新编码。

- 签名输入为 `chast-access-ticket-v1\0` 加 envelope 原始字节，算法 Ed25519。线上形式为 `envelope || 64 字节签名`；JSON 传输用 base64url，v62 协议消息用原始字节。
- envelope 头：`u8 version=1`、`u8 algorithm=1`、`u16 flags=0`，全部大端。
- 字段顺序固定，字符串为 `u16` 大端长度前缀加 UTF-8 字节：`issuer`（1–128）、`audience`（1–128）、`kid`（1–64）、`host_id`（1–64）、`host_public_key_fingerprint`（1–128）、`subject`（1–128）；随后 `client_public_key` 固定 32 字节（无长度前缀）、`permissions` `u32`、`policy_version` `u32`、`iat` / `nbf` / `exp` 各 `i64`（Unix 秒）、`jti`（1–64）。
- envelope 上限 1024 字节，完整票据上限 1088 字节。未知 version/algorithm、flags 非零、必填字段为空、字段超长、负时间、尾随字节、全零客户端公钥一律拒绝。
- 时间约束：`iat > 0`、`nbf >= iat`、`exp > nbf`、`exp - iat <= 60s`；验证允许 ±5s 时钟误差。签发时 `iat`/`nbf` 不得超前当前时间 5s 以上，`exp` 不得早于当前时间。
- 验证顺序：长度与 envelope 严格解析 → 按 `kid` 查预置公钥（未知即拒绝，不自动下载）→ 对原始 envelope 验签 → `issuer`/`audience` 精确匹配 → 时间窗口。`jti` 由服务端 `ReplayGuard` 消费；缓存满时拒绝新准入，不淘汰未过期条目。
- 签发私钥独立于部署 `host_key`/`link_key`/管理 key 和服务端绑定身份；主机只固定 `kid` 公钥。
- 黄金向量：seed = `00..1f`（32 字节），issuer=`chast.example`，kid=`kid-2026-11`，audience=`device:devbox`，host_id=`devbox`，fingerprint=`SHA256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef`，subject=`alice`，client_public_key=`0x40..0x5f`，permissions=`0x0000000f`，policy_version=`2`，iat=nbf=`1794225600`，exp=`1794225660`，jti=`jti-0001`。指纹字段按字符串传输，但比较以 64 位十六进制摘要为准：忽略可选的 `SHA256:` 前缀、`:` 分隔符和大小写；签发端把裸摘要规范为 `SHA256:<小写十六进制>`。其余字段仍逐字节或按精确字符串比较。
  - envelope hex：`01010000000d63686173742e6578616d706c65000d6465766963653a646576626f78000b6b69642d323032362d31310006646576626f7800475348413235363a303132333435363738396162636465663031323334353637383961626364656630313233343536373839616263646566303132333435363738396162636465660005616c696365404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f0000000f00000002000000006af1b5c0000000006af1b5c0000000006af1b5fc00086a74692d30303031`
  - signature hex：`e907b6f1daaf566b88b482132e197190803bdb5e1d87b5c3100eb3bda6a993799033f6b3b09ab233dc61d3a82ccc8d70f78f2b2f6df5634794cdec5b31bad504`
- 当前已完成 Go 侧签发/校验/重放缓存与自动化、服务端的账号换票端点与 lookup 内联签发（账号 + `client_public_key`，`TestLookupAccessTicket`），C++ 校验库与 v62 主机准入（第 7 节，含票据、持有证明与挑战），以及主机对票据会话的 60 秒截止（`host_ticket_lease`）；回环自动化覆盖主机准入的允许与否定路径。查看器已持久化客户端密钥并在启动时在线取票、按 20 秒自动续租（第 7 节）；主机已支持重叠 `kid` 的签发密钥轮换，撤销推送仍未接入，因此票据格式冻结与校验库存在不等于授权链路完成。

## 7. v62 准入消息与持有证明 v1（2026-11-10）

实现：`include/chast/protocol.h`、`src/net/protocol.cc`（消息编解码）、`src/net/quic.cc`（握手次序与续租消息）、`include/chast/host_security.h`、`src/host/host_security.cc`（验票、证明与续租校验）、`include/chast/access_ticket.h`、`src/host/access_ticket.cc`（证明消息、签名与客户端密钥派生）、`src/host/host_server.cc`（会话循环应用续租）、`app/windows/viewer/viewer_engine.cc`（票据队列与续租调用）、`app/windows/viewer/viewer_bridge.cc`（DPAPI 客户端密钥与消息通道）、`app/lib/server_api.dart`（票据解析与 lookup 参数）、`app/lib/pages/session_page.dart`（取票与 20 秒定时）。自动化：`tests/protocol_test.cc`（v62 消息长度与往返）、`tests/managed_access_test.cc`（回环准入）、`tests/host_ticket_lease_test.cc`（到期与续租）、`tests/access_ticket_test.cc`（客户端密钥派生与证明）、`app/test/server_api_test.dart`（票据 lookup 解析）；查看器原生目标由 `chast-viewer-probe` 与 Flutter `chast_viewer` 构建验证。真实链路的受管准入尚未验收。

- 交接次序固定：查看器在控制流上发出的第一条消息是 `AuthBegin`（类型 80，空载荷）；主机收到后回 `AuthChallenge`（类型 79，恰好 32 字节随机数）。QUIC 客户端发起的流在首个字节到达前对主机不可见，因此必须由查看器先开口；同一条连接上重复的 `AuthBegin` 重发同一挑战，不重新生成。两条消息单独都不构成凭据。
- Hello 末尾追加两项，顺序固定：`u16` 票据长度（≤1088）+ 票据字节（`envelope || 64 字节签名`），随后 `u16` 证明长度（必须 64）+ Ed25519 证明。票据与证明只能同时出现；出现票据而无证明、或证明在前，解析失败。未受管连接两者都不携带。
- 持有证明消息：`"chast-access-proof-v1\0"` + `u16` host_id + host_id + `u16` 指纹摘要 + 摘要（能解析出 64 位十六进制时统一为大写，否则原样）+ 32 字节主机挑战 + `u16` 客户端 nonce 长度 + Hello 的 16 字节 nonce + SHA-256(带签名的票据)，用票据 `client_public_key` 对应的私钥签名。
- 主机校验顺序：策略标记可读且受管 → 已固定签发公钥 → 必须携带票据 → 逐字节解析并按 `kid` 验签（issuer/audience/host_id/设备指纹/±5 秒时间窗）→ 票据 `policy_version` 等于共享标记 → 主机挑战非全零 → 持有证明 → 通过后才消费 `jti`。证明失败不消耗票据，也不进入口令失败计数。票据通过后仍执行本地密码、审批、TOTP，权限取票据 ∩ `permission_ceiling` ∩ 本机默认 ∩ 本次请求。
- 查看器侧从票据 envelope 取 `host_id` 与指纹生成证明；票据无法解析或缺少 32 字节客户端密钥时以 `InvalidCall` 在本机拒绝，不发送 Hello。受管主机对不带票据的 Hello 一律 `Declined`，不回退到口令或旧传输凭据。
- lookup 内联签发（2026-11-10）：账号鉴权的 lookup 在携带 `client_public_key`（base64url Ed25519，32 字节）时对受管设备直接返回票据。它与换票端点共用 `mintAccessTicket`：请求权限与策略上限取交集，`jti` 在签名前进入重放缓存，有效期 60 秒；未配置签发密钥 503、共享凭据 403、坏密钥或坏权限 400、未纳管不返回票据，签发记审计。自动化 `TestLookupAccessTicket`（`server/internal/api/access_tickets_test.go`）。
- 主机租约截止（2026-11-10）：会话开始时按票据 `exp` 计算单调时钟截止（剩余为负按 0），stream/file/control 三个循环每轮检查；到期记录 `the access ticket expired; closing the session` 并以 `ticket_expired_` 标记，结束宽限压到 500 ms，不再等待默认 15 秒的 Stats/Bye 统计。仅作用于 `via_access_ticket && ticket_expires_at > 0` 的会话，口令/未纳管会话不受影响。自动化 `host_ticket_lease`（`tests/host_ticket_lease_test.cc`）用 3 秒票据覆盖准入、到期前存活、到期断开与主机回到 Listening；真实链路仍待人工验收。
- 票据续租（2026-11-10）：查看器在既有控制流上发 `TicketRenew`（81），载荷为 `u16` 票据长度（1..1088）+ `envelope || 签名`、`u16` 证明长度（必须 64）+ 证明；证明与握手同格式，但摘要覆盖新票据：`"chast-access-proof-v1\0"` + host_id + 指纹 + 32 字节主机挑战 + 16 字节 Hello nonce + SHA-256(新票据)，用新票据 `client_public_key` 的私钥签名。主机 `HostSecurity::renew_ticket` 重新读取受管策略标记并按 `kid` 验签、校验时间窗与版本相等，再要求票据主体等于已准入主体、`client_public_key` 等于准入时的会话密钥、挑战非零、持有证明正确，全部通过后才消费新 `jti`；过期票据无法续租，因此网络失败不会延长原截止时间。接受后会话权限取票据 ∩ `permission_ceiling` ∩ 本机默认并与当前掩码逐位取交集（续租只能收窄，不能越过操作员已批准的权限），收窄走 `revoke_policy_permissions`/`apply_permission_policy`，即使本机禁用操作员改权限也生效；截止时间改为新 `exp` 的单调时钟剩余值（剩余为负按 0），并以 `TicketRenewResult`（82）返回新到期秒数与生效掩码。拒绝时回带原因的结果、记录 `the ticket renewal was refused: ...`、以 `ticket_renew_failed_` 将结束宽限压到 500 ms 并关闭会话；非票据会话或握手未完成时续租按无效调用拒绝。自动化：`tests/protocol_test.cc` 的 `test_ticket_renew` 覆盖往返与坏长度/坏码/坏掩码/坏原因；`host_ticket_lease` 用首个 3 秒票据在 +1.2 秒换成 +6 秒票据，验证 +4 秒仍存活、约 +7 秒到期，并用异客户端密钥的合法票据验证拒绝（`PermissionDenied`）与会话关闭；第四阶段用零权限票据续租，验证查看器收到权限更新、后续文件列表被主机以 `file transfer is disabled` 拒绝且会话保持到新截止。查看器已在启动时在线取票并按 20 秒自动续租（下一条），撤销推送仍未实现，真实链路待人工验收。
- 签发密钥重叠（2026-11-10）：主机 `host.ini` 在原有 `access_issuer/access_kid/access_public_key` 之外接受可重复的 `access_key=<kid>:<base64url Ed25519 公钥>`（最多 8 个附加项）。`HostSecurity` 把主 pin 与附加项一并装入 `AccessTicketVerifier`；任一附加 pin 格式错误、或主 pin 只写一半，都让校验器不构建并以受管主机失败关闭处理。未知 `kid` 仍直接拒绝，不会自动下载。轮换步骤：1) 离线生成新种子并取得公钥；2) 向主机追加 `access_key=<新 kid>:<新公钥>` 并应用；3) 服务端以新 `--access-kid/--access-key` 重启签发；4) 重叠期结束后从主机移除旧主 pin（或把新 kid 提升为主 pin）。自动化：`host_settings_test` 覆盖附加项往返、非法格式与主 pin 校验；`host_security_test` 用第二签发密钥验证重叠 `kid` 准入、移除主 pin 后新 `kid` 继续工作、旧 `kid` 被拒。
- 查看器取票与 20 秒续租（2026-11-10）：查看器首次需要票据时生成 32 字节 Ed25519 种子，用 DPAPI 以当前 Windows 用户保护后写入 `%APPDATA%\chast\access-ticket.key`（缺失或损坏时重新生成并覆盖，明文在写盘后清零），公钥以 base64url 经 `ticketIdentity` 通道返回。`ServerApi.lookup` 在账号身份下携带 `client_public_key` 查询参数并沿用账号 bearer；`HostInfo.accessTicket` 解析 `access_ticket`（缺失或空字符串视为未纳管）。`SessionPage` 仅在 `viaServer` 且账号有效的会话启动前取票：未受管返回空票据走本机认证，票据已过期以可读错误中止，取票失败不进入重连循环。票据以 base64url 经 `start` 通道交给原生层，bridge 解码后与种子一起传给引擎，引擎只在 Hello 中携带；种子不离开原生进程。连接成功后 Dart 每 20 秒重新 lookup 并用 `renewAccessTicket` 把新票据排入引擎（互斥量保护的“最新覆盖”队列，避免阻塞 UI）；引擎循环每轮取出并以 3 秒超时调用 `QuicSession::renew_access_ticket`，成功打印 `viewer: access ticket renewed`，失败打印原因并等待主机按租约截止关闭。续租 lookup 收到 401/403 时取消定时并主动 `stop`，不等主机超时。会话超过 30 分钟访问令牌寿命时，页面先复用其他窗口已持久化的刷新结果，否则用刷新令牌自续并把新会话写回共享存储，续租与结束审计不会因令牌到期中断。自动化：`app/test/server_api_test.dart` 覆盖带公钥 lookup 的路径、`client_public_key` 查询、账号 bearer 与票据字段解析，以及空票据忽略；`chast.access` 的客户端密钥派生/证明由 `tests/access_ticket_test.cc` 覆盖；原生代码经 `chast-viewer-probe` 与 Flutter `chast_viewer` 两处 Release 构建验证。撤销推送与同公钥新 kid 轮换已完成，真实主机单机回环已通过；真正更换私钥、生产反向代理/中继、双机 Flutter 与 PostgreSQL 仍待验收。
- 运行中敏感操作撤权（2026-11-10）：主机所有敏感请求的 handler 都会在出队时重读 `session_->permissions()`，而不是仅依赖查看器按钮或客户端本地检查：重启/锁屏/阻止输入、远程配置、OS 登录、会话列表/切换、隐私模式逐项校验对应位；端口转发与终端在 transport 准入时同样校验。权限收紧时 `QuicSession::set_permissions_impl` 先改本地掩码再通知：释放按住键、中止麦克风/摄像头/剪贴板/文件流、断开终端与端口转发、清空目标注册；桌面循环另外在掩码变化后解除本地输入阻止并把隐私模式恢复为关闭。自动化 `host_parity` 新增“running services stop when their permission is revoked”（`--revocation`）：先通过回环 TCP 服务完成端口转发收发并启动 ConPTY 控制台执行命令，再一次性撤销 `kPermissionPortForward | kPermissionTerminal`，验证查看器收到能力更新、后续转发字节和控制台输入被拒、主机侧转发套接字被关闭、主机日志出现 `terminal access was revoked` 且控制台进程被 relay 停掉。阻止输入、剪贴板、文件、摄像头、麦克风（`host_voice`）与隐私模式（`display_privacy_live`，需真实显示）各有既有自动化/实机测试；真实链路待人工验收。

## 8. G04 集中策略对象与分配（2026-11-10）

实现：`server/internal/account/policy_objects.go`（对象/分配校验、解析、物化与重算）、`server/internal/account/device_policy.go`（来源与单调版本）、`server/internal/account/migrate.go`/`migrate_postgres.go`（schema v13/v14）、`server/internal/api/policy_objects.go`（管理 API、审计、重算与事件）、`server/internal/webadmin/assets`（三块界面）与 `webadmin_test.go`。

### 8.1 数据模型与解析

- `policy_objects`：名称唯一（≤64 字符）、`open`/`restricted`、32 位权限上限、允许用户/组、对象版本与更新者。每次编辑递增对象版本；对象只描述“想要什么”，不是准入记录。
- `policy_assignments`：把对象绑定到 `all`、`group` 或 `device`；`all` 的范围值为空，其余为合法 ID。同一 `(scope_kind, scope_value)` 唯一，再次分配返回 409，必须先删除原分配。优先级范围 -10000..10000。配额 256 个对象、1024 条分配。
- 解析与物化：设备可能同时命中 `all`、多个分组和一台设备分配。按优先级降序选择；优先级相同按具体程度 `device > group > all` 决胜，同级仍并列时按分配创建顺序取先创建者。结果写入 `device_policies`，并记录 `source`（kind/id/name/version/priority）供控制台显示。
- 手动覆盖：沿用 `PUT /v1/device-policies/{peer}` 的现有语义。手动行没有来源，优先于所有分配；`DELETE /v1/device-policies/{peer}` 删除手动行后立即重新解析匹配分配（或回到未纳管）。删除仍被分配引用的策略对象返回 409。
- 版本单调：设备生效版本不再等于对象版本。schema v14 的 `device_policy_versions` 为每台设备持久保存最近一次物化版本，`nextDevicePolicyVersionTx` 在同一事务内分配 `max+1`；因此设备分配覆盖、回退、对象编辑、删除后重建和服务器升级都不会把版本降回去。Go 代理明确忽略低于/等于当前版本的策略，这一约束是回退安全的前提。

### 8.2 API、审计与事件

- 管理 API（均为 admin `devices` 资源）：`GET/POST /v1/policies`、`GET/PUT/DELETE /v1/policies/{id}`、`GET/POST /v1/policy-assignments`、`DELETE /v1/policy-assignments/{id}`。错误映射：未知对象/分配/分组 404；对象重名、范围已有分配、对象仍被引用 409；非法名称/模式/范围/优先级、未知用户/组、超配额 400。写操作记 `policy_object_create/update/delete`、`policy_assignment_create/delete` 审计。
- 可移植 JSON：`GET /v1/policies/export` 以 `format=chast-policy-objects`、`version=1` 导出名称排序的对象，不携带 id、版本、时间戳或更新者；`POST /v1/policies/import` 支持默认 `conflict=reject` 和同名内容更新 `conflict=update`。导入先完成整包校验，再在单事务内创建/更新；`update` 对内容相同的对象返回 `unchanged` 且不升版本。请求体限 1 MiB，严格拒绝未知字段和尾随 JSON，单包最多 256 个对象；导入不包含分配，成功写 `policy_object_import` 审计。
- 变更传播：创建/更新/删除对象或分配、以及手动策略写入/删除后，服务端重算受影响设备（对象编辑与分配删除未限定设备时全量重算），把变化的行作为 `policy` 事件、把变为未纳管的设备作为 `managed=false` 事件发布到既有 `GET /v1/hosts/{id}/events` 长轮询。重算失败只记日志，已提交的变更由下一次注册或重算补齐；准入路径本身不依赖推送必达。
- 注册一致性：每次主机注册仍从 `device_policies` 读取当前生效策略并随注册响应下发；受管设备只有在共享文件版本与策略一致时才准入，回执版本不一致时控制台显示“待应用”。

### 8.3 管理台

“设备策略”页拆为三块：策略对象（名称、模式、权限上限、允许用户/组、对象版本、编辑）、策略分配（对象、范围、优先级、删除）、设备策略（生效结果）。生效结果新增“来源”列：手动覆盖显示“手动覆盖”，分配来源显示“策略 <名称> · v<对象版本>”并以标题提示 kind/id/priority；主机回执仍按“版本相等且 applied=true”显示“已应用版本”，否则“待应用/应用失败”。界面继续满足 CSP 约束（无内联脚本/样式、无 `innerHTML`），`webadmin_test.go` 与 `app.js` 的 DOM 构建方式锁定这一点。

### 8.4 验证与边界

- 自动化：`go build ./...`、`go vet`（api/account/webadmin）、`go test ./...` 全绿，含策略存储、HTTP 路由、事件推送、版本序列迁移、管理台 CSP/接线断言；`TestImportPolicyObjects*` 与 `TestPolicyObjectImportExportRoutes` 覆盖导入整包校验/事务回滚、`reject`/`update` 分类、未变不升版本、1 MiB 与 256 对象上限、导出元数据边界、往返导入、设备策略 reconcile 和审计。
- 真实链路（`server/build/g04-e2e/run.sh`，被忽略目录）：生产 `chast-server` + 真实 `chast-tunnel host` 共 26 项通过——分配注册下发 v1（open/5）、共享策略文件、来源元数据；设备范围 restricted 分配推送 v2 并覆盖；删除设备分配后回退到 all 并分配单调 v3；签名回执 v1/v2 被接受且旧回执不能确认新版本；删除最后一个分配后代理写 `managed=false`、服务端详情为 `{"managed":false}`。
- 浏览器冒烟：真实登录后创建对象/分配、设备覆盖、删除覆盖触发回退（v2 来源显示“策略 bench-open · v1”），并验证分配对话框的 `group`/`device` 分支会切换范围值输入、datalist 与提示。
- 未覆盖：生产 PostgreSQL 部署上的完整 ACL 整链；生产反向代理/中继与双机 Flutter 组合；多操作者并发编辑与大规模全量重算的性能；跨设备事务一致性未做分布式语义。PostgreSQL 13/14 的存储集成、迁移、版本间恢复与双实例冷启动已通过，见 [PostgreSQL 13/14 真库验收](postgres-acceptance-2026-11-14.md)。真实 `chast-host.exe` 接收物化策略、执行受管会话与撤权已在 2026-11-13 单机回环中覆盖，见 [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md)。
