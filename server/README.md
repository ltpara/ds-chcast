# chast-server — 会合、打洞与中继

Go 实现的 chast 连接服务：持久主机身份、查看者寻址、UDP 打洞协调和不可达时
的中继/载体回退。服务端只转发加密的 QUIC 数据报，不解析、不终止 chast 协议，
因此协议 v23 的令牌、审批、证书指纹和输入通道都保持端到端语义。

参考实现是 RustDesk 的 `hbbs`（会合/信令）与 `hbbr`（中继）：

| RustDesk | 本服务 | 说明 |
| --- | --- | --- |
| `RegisterPk` 首次登记 ID 公钥 | `POST /v1/hosts` + Ed25519 注册签名 | 公钥与 ID 永久绑定，续约必须用同一私钥签名 |
| `Peer` 记录持久化 | `chast-server-state.json` + 服务器签名 `binding` | 离线仍可查询身份，查看者用固定的服务器公钥校验 |
| `PunchHoleRequest` / `PunchHole` | UDP `punch_request` / `punch_peer` | 交换观察到的公网地址并让双方互发探针 |
| `FetchLocalAddr` | `local_addrs` + `nat_type` | 主机上报局域网候选与 NAT 分类 |
| `RequestRelay`（uuid 配对） | 按主机分配 UDP 中继端口 | 查看者直接连该端口，主机侧代理回连中继 |
| 共享中继端口按 UUID 复用 | `--carrier-listen` TCP/WebSocket 共享载体 | 一个端口承载所有主机链路，查看者凭随机令牌选择主机 |
| 中继按 uuid 配对 TCP 流 | 透明转发 UDP 数据报或载体帧 | 不需要拆 QUIC 流，握手与加密都在端点上 |
| `IP_CHANGES` 短期地址审计 | `ic` 管理命令 + `chast_ip_change_*` | 首次观察只建基线；窗口内出现多个地址的主机列入审计 |
| `relay-servers` 与 `get_relay_server` 轮换 | `--relay-servers` + `rs`/`tg` + 注册 `relays` | 每个中继端口按租约地址重新绑定监听，回包源地址与查看者拨号地址一致 |
| `check_relay_servers` + 独立 hbbr 进程 | `--relay-nodes` + `chast-relay` | hbbs 探测节点健康并向其申请租约；节点独立承担 UDP/TCP 中继与共享载体，故障时切换或回退本机 |
| `blacklist.txt`/`blocklist.txt`、带宽限制与降级 | 中继策略 + `ba/br/b`、`Ba/Br/B`、`sb/tb/ls/dt/t/u` | 单会话与总带宽、黑名单慢速档、阻断名单、自动降级；本机中继与独立节点共用同一实现，名单可落盘 |

## 设计约束

1. **chast 的传输是 QUIC（MsQuic），视频与控制都在 QUIC 流里**。中继如果按
   TCP 字节流配对，就必须终止 QUIC 或在客户端改造传输层。这里选择 UDP 数据报
   转发：每个主机获得一个专属中继 UDP 端口，查看者把该端口当作普通 QUIC 服务
   端地址；中继把数据报原样送到主机侧代理，由主机侧代理再送到
   `chast-host` 的监听端口。QUIC 连接仍是查看者 ↔ `chast-host`，中继看不到流
   边界，也无需理解 chast。UDP 被封时，同一数据报可以装进 TCP 帧或 WebSocket
   binary message 经“载体”通过，QUIC 端点无感知。
2. **MsQuic 不暴露内部 UDP 套接字**，无法在 QUIC 自己的套接字上做 STUN/打洞。
   因此 NAT 穿透由两侧的 `chast-tunnel` 代理完成：代理各自持有一个对外 UDP
   套接字，用该套接字同时做保活、探测和真实数据转发。直连时数据不经过服务器；
   中继时服务器只做无状态转发。
3. **同一时刻一个主机只服务一个查看者**（现有 `HostServer` 的语义）。中继端口
   与共享载体因此只维护一个活跃查看者，另一个来源在活跃期内会被拒绝（`busy`），
   空闲后才可接管，避免两路 QUIC 数据报混在一起。
4. **服务端不信任客户端自报的地址**。公网映射只取 UDP 观察值；主机上报的
   局域网候选仅作为同网段提示。
5. 服务器**从不接触 chast 令牌和证书指纹的明文语义之外的内容**。HTTP 注册可
   携带展示用指纹；连接令牌始终只存在于 QUIC 握手内部。共享载体的
   `carrier_token` 只授权“在该主机的中继槽位上收发数据报”，不参与 QUIC 认证。
6. **每个数据面都做一次性挑战**。keepalive、UDP 中继和 TCP/WebSocket 载体分别
   使用 30 s 有效的随机 nonce，签名覆盖 ID、端口与观察地址，服务端只保存公钥。
7. **本机中继地址列表属于单个 chast-server 实例**。chast 把 UDP 中继端口绑在主机
   租约上，所以 `--relay-servers` 的每一项都必须是本实例的可达地址（多网卡、
   多个域名或 NAT 映射），端口在注册时拼上。地址轮换只影响新租约；每个中继
   端口在注册时按租约地址重新绑定监听，使回包源地址等于查看者拨号地址。
8. **跨机中继走独立节点，不在 hbbs 之间共享端口**。`--relay-nodes` 里的每个
   `chast-relay` 自己持有端口集合与共享载体，hbbs 只下发租约地址；节点用
   hbbs 下发的公钥和载体令牌在本地完成挑战校验，不需要回连 hbbs。全部节点
   不可用时 hbbs 回退本机中继（RustDesk 的 hbbs 此时直接失败），因此控制面
   不可用不会让新注册彻底失败。

## 拓扑

```
                HTTP 8443/tcp 注册、查询
  chast-tunnel host  ───────────────────────────┐
        │  UDP 保活（签名挑战）                  │
        │  UDP 8444 会合                         │   chast-server
        │  UDP HELLO + 数据 ↔ 41000..41099 中继  │
        │  TCP/WS 8445 共享载体（可选）          │
        ▼                                        │
  chast-host :4433 (QUIC)                        │
        ▲                                        │
        │ 直连（打洞成功后，同一数据套接字）     │
        │ 或经中继/载体（主机的出站映射）        │
  chast-tunnel viewer ───────────────────────────┘
        ▲ 127.0.0.1:24434 (UDP)
        │
  chast-quic / Flutter 查看者
```

跨机部署时，把 hbbs 的 `--relay-nodes` 指向一个或多个 `chast-relay`；主机与
查看者仍然只认注册/查询响应里的 `relay` 与 `carrier`，中继端口和共享载体由节点
提供，本机 `--relay-servers` 与 `--carrier-listen` 保留为节点全部不可用时的
回退路径。

主机代理：

- 用 `POST /v1/hosts` 提交 `id`、Ed25519 公钥、可选指纹、时间戳与注册签名；
  私钥由 `--key` 派生，从不上传。首次登记后公钥永久绑定；
- 用数据套接字向 UDP 会合端口发送 keepalive；服务器回
  `keepalive_challenge`，代理签名后发 `keepalive_auth`，服务器记录其
  **观察到的公网地址**、`local_addrs` 与 `nat_type`；
- 用同一套接字向分配到的中继端口发送 `relay_hello`，经 `relay_challenge` /
  `relay_auth` 认证后，服务器记录主机侧中继地址；注册响应的 `relays` 在本机
  模式下列出全部本地地址、节点模式下只含该节点租约端点，代理接受其中任意
  地址（含同一主机的其他解析结果）发来的挑战，避免 DNS 轮换或多地址部署
  把主机钉在单一入口；
- 注册响应包含 `carrier` 时，优先向共享载体建立 `CHASTT-HOST2` 链路；没有
  共享载体则回退到中继端口的 TCP 载体；
- 收到 `punch_peer` 后向查看者候选地址发探针，打开本侧 NAT；
- 收到的查看者数据报（直连来源、中继来源或载体帧）转发给 `--local` 指定的
  `chast-host`；`chast-host` 的回包发回最后一个远端来源。由于两侧代理都只有
  一个数据套接字，直连切中继时 QUIC 看到的五元组不变。

查看者代理：

- 用 `GET /v1/hosts/{id}` 查询在线状态、主机候选/局域网地址、NAT 类型、中继
  地址、会合地址、身份绑定与 `transport`（`direct`/`relay`/`carrier`）；
  查询返回的 `relay` 是该租约的主中继地址，续约不变，查看者无需在多个
  地址间自行选择；主机在线时还会得到共享载体地址与 `carrier_token`；
- `relay_only` 为真时服务器不返回任何直连候选，`nat_type` 固定为
  `symmetric`；此时 `direct` 模式直接报错，`auto` 只会选择载体或中继；
- `auto` 模式在有候选时先向主机候选地址发真实 QUIC 数据报并打洞，
  `--direct-timeout` 内收到主机回包就锁定直连，否则切到中继端口并保持；
  `transport=carrier` 时直接使用共享载体；
- `relay` 优先 UDP 中继，主机只报告 `transport=carrier` 时改用载体；
  `carrier` 模式强制 `CHASTT-V2` 共享载体，链路断开按 0.5 s→5 s 退避重连，
  空闲每 30 s 发空帧保活；
- `direct` / `relay` / `carrier` 模式用于强制验证各条路径。

局域网内查看者也可以不经过代理，直接用查询到的候选地址连接 `chast-host`；
代理主要解决跨网直连和中继回退。

## 控制协议

所有控制报文都是 **8 字节魔数 `CHASTC1\n` + UTF-8 JSON**，走 UDP；不以魔数开头
的数据报一律视为不透明数据。控制报文只由代理和服务器处理，绝不转发给
`chast-host`。

| type | 方向 | 字段 | 用途 |
| --- | --- | --- | --- |
| `keepalive` | 主机 → 会合 | `host_id` | 请求保活挑战 |
| `keepalive_challenge` | 会合 → 主机 | `nonce`, `candidate` | 一次性 nonce 与服务器观察地址 |
| `keepalive_auth` | 主机 → 会合 | `host_id`, `nonce`, `candidate`, `local_addrs`, `nat_type`, `signature` | 签名应答并上报候选 |
| `keepalive_ack` | 会合 → 主机 | `ok`, `candidate`, `relay`, `local_addrs`, `nat_type`, `error` | 认证结果与中继地址 |
| `punch_request` | 查看者 → 会合 | `host_id`, `nonce` | 请求与主机交换候选地址 |
| `punch_peer` | 会合 → 双方 | `nonce`, `candidate`, `relay`, `public_key`, `binding` | 对方地址与身份绑定 |
| `punch` | 代理 ↔ 代理 | `nonce` | 打开 NAT 映射的探针，收到即说明路径可用 |
| `relay_hello` | 主机 → 中继 | `host_id` | 请求中继挑战 |
| `relay_challenge` | 中继 → 主机 | `nonce` | 一次性 nonce |
| `relay_auth` | 主机 → 中继 | `host_id`, `nonce`, `signature` | 绑定中继端口与主机侧地址 |
| `relay_hello_ack` | 中继 → 主机 | `ok`, `error` | 认证结果 |
| `busy` | 中继 → 查看者 | `error` | 活跃查看者占用中继端口 |

签名消息都用长度前缀字段编码，域分别是 `chast-keepalive-v1`、
`chast-relay-v1`、`chast-carrier-host-v1` 和 `chast-register-v1`：

```text
keepalive : domain | host_id | candidate | nonce
relay     : domain | host_id | relay_port | candidate | nonce
carrier   : domain | host_id | candidate | nonce
register  : domain | host_id | fingerprint | unix_timestamp
```

### 共享载体（TCP 8445 / WebSocket）

`--carrier-listen 0.0.0.0:8445`（空值禁用）让所有主机共用一个 TCP 端口；
`GET /chast` 完成 RFC 6455 握手（子协议 `chast-carrier.v1`）后走 binary
message，其余连接按首字节区分原始 TCP：

| 角色 | 握手 |
| --- | --- |
| 主机 | `CHASTT-HOST2\n` + `{"host_id":…}` → `CHASTT-CHALLENGE1 {nonce}` → `CHASTT-AUTH1 {host_id,nonce,signature}` → `CHASTT-OK2\n` |
| 原始查看者 | `CHASTT-V2\n` + `{"host_id":…,"token":…}` |
| WS 查看者 | `GET /chast`，首条 binary message 为同样的 JSON hello |

握手成功后每个方向都是 `u32le length | payload` 帧（WebSocket 的 binary
message 本身即 payload），单帧上限 64 KiB。令牌来自认证查询的
`carrier_token`，只在该主机在线时返回；同一主机的旧链路被新链路替换时，
其查看者随之关闭。忙、令牌错误和 malformed hello 都会直接关闭连接并计数。
查看者的空帧是保活，服务器回一个空帧，双方都能借此识别静默断链；
主机侧的空帧同样由服务器应答。

已认证的主机链路也计入在线判定：即使 UDP keepalive 全部丢失，查询仍返回
`online:true`、有效的 `carrier_token` 与 `transport:"carrier"`，并且不再下发
过期的 UDP 候选；主机链路断开后（读超时 90 s），只有 UDP keepalive 也不新鲜时
查询才恢复为离线。

### HTTP

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| `POST` | `/v1/hosts` | `Authorization: Bearer <server-key>`（配置了才需要）+ 主机注册签名 | 注册或续约，返回 `relay`、`relays`、`punch`、`binding`、`carrier`、`carrier_token` |
| `GET` | `/v1/hosts/{id}` | 无 | 身份、绑定、在线状态、候选、NAT、租约中继/会合地址、`transport` 与 `relay_only`；共享载体与令牌只在该主机在线时返回 |
| `DELETE` | `/v1/hosts/{id}` | Bearer server-key | 主机主动下线并释放中继端口，保留永久身份 |
| `DELETE` | `/v1/identities/{id}` | Bearer server-key | 管理员重置丢失密钥的主机身份 |
| `GET` | `/v1/server` | 无 | 服务器公钥、会合地址、共享载体、中继地址列表、独立节点健康、TTL、是否启用 TLS |
| `GET` | `/v1/stats` | 配置了 server-key 时需要 | 注册数、身份数、中继与共享载体计数、审计计数、中继地址数、`relay_nodes` 与节点健康 |
| `GET` | `/metrics` | 配置了 server-key 时需要 | Prometheus 文本格式指标 |
| `GET` | `/healthz` | 无 | 存活检查 |

主机私钥由 `--key` 派生（`SHA-256("chast-host-identity-v1\n" + key)` 作为
Ed25519 种子），服务端只保存公钥与可选指纹。首次注册把 ID 永久绑定到该公钥；
续约必须由同一私钥签名，指纹也只在首次登记时可采用，防止他人用同一 ID
换绑或劫持中继端口。`DELETE /v1/identities/{id}` 是丢失密钥时的人工出口。

示例：

```json
POST /v1/hosts
{"id":"devbox","public_key":"<base64url>","fingerprint":"AB12...",
 "timestamp":1761300000,"signature":"<base64url>"}
→ 200 {"id":"devbox","relay":"203.0.113.5:41007",
       "relays":["203.0.113.5:41007","203.0.113.6:41007"],
       "punch":"203.0.113.5:8444",
       "expires_in_sec":60,"public_key":"<base64url>",
       "server_public_key":"<base64url>","binding":"CHASTB1...",
       "carrier":"203.0.113.5:8445","carrier_token":"<32 hex>"}

GET /v1/hosts/devbox
→ 200 {"id":"devbox","online":true,"candidate":"198.51.100.7:51234",
       "candidates":["198.51.100.7:51234","192.168.1.7:51234"],
       "local_addrs":["192.168.1.7:51234"],"nat_type":"asymmetric",
       "relay":"203.0.113.5:41007","punch":"203.0.113.5:8444",
       "fingerprint":"AB12...","public_key":"<base64url>",
       "server_public_key":"<base64url>","binding":"CHASTB1...",
       "carrier":"203.0.113.5:8445","carrier_token":"<32 hex>",
       "last_seen_ms":1200,"expires_in_ms":48000}
```

离线时查询仍返回 `id`、`online:false`、公钥、指纹、绑定与 `punch`，但不再返回
候选、共享载体与 `carrier_token`，避免把仍有租约价值的接入凭据暴露给所有人。

`binding` 是服务器用持久 Ed25519 身份签署的
`{"v":1,"id":…,"pk":…,"fp":…,"iat":…}`，格式为
`CHASTB1.<payload>.<signature>`（base64url）。查看者固定
`/v1/server` 的 `public_key` 后即可独立验证，不必只信任查询响应。

## NAT 行为与边界

- 公网地址由服务器在 UDP 会合端口观察得到，不信任客户端自报；主机的候选地址
  是其数据套接字到会合端口的映射。主机随后把同一套接字在中继端口上的映射与
  它会合映射比较：端口一致记为 `asymmetric`（锥形 NAT），不一致记为
  `symmetric`，尚无法判断时为 `unknown`。
- 主机代理从绑定套接字枚举本机单播地址，作为 `local_addrs` 随 keepalive 上报；
  查询的 `candidates` 会把公网观察地址与局域网地址去重合并。局域网地址仅为
  提示，服务端不会用它替代观察地址。
- 双方从**同一个**数据套接字向对方候选地址发探针，打洞对锥形 NAT
  （全锥/受限/端口受限）有效。对称 NAT 下不同目的地的映射端口不同，直连会
  失败，查看者代理在 `--direct-timeout` 后自动切中继；这是有意保留的回退，
  不伪装成"总能直连"。
- 中继端口按主机分配，服务器重启后重新分配；主机代理会在续约时更新地址。
  `--state` 会把身份写入 `chast-server-state.json`，重启后恢复 ID 绑定（租约与
  中继端口不持久化，主机重新注册时再分配）。
- `--relay-servers a,b` 控制注册返回的中继入口：按轮询为新租约选择 `relay`，
  同时用 `relays` 下发全部端点（已拼上该租约的端口）。续约沿用
  `Host.RelayHost`，不会因刷新而漂移；地址被 `rs` 移除后才重新选择。
  每个中继端口在注册时用 `relay.Manager.BindHost` 按租约地址重新绑定 UDP/TCP
  监听，所以回包源地址等于查看者拨号地址；地址无法在本机绑定（例如域名指向
  别的机器）时退回 `--relay-listen` 并记录警告。列表属于本实例，不能用来
  指向独立的中继节点。
- 中继是 UDP，无拥塞控制；chast 的 QUIC 自身有拥塞控制与重传。中继会对每个
  端口限制活跃查看者数量为 1，并限制控制报文速率，降低开放反射风险。
- QUIC 初始包被填充到至少 1200 字节，中继按原样转发，不放大流量。

## 安全

- **传输安全**：chast 的 QUIC TLS 证书、指纹 pinning、一次性/可复用令牌和
  连接审批完全在端点上完成，服务器、中继或载体都无法解密视频与控制消息。
- **主机身份**：ID 与 Ed25519 公钥永久绑定；注册、keepalive、UDP 中继和
  TCP/WS 载体各自验证一次性挑战签名，nonce 30 s 过期且单次有效。服务端只
  保存公钥，`--key` 派生出的私钥不离开主机。
- **身份绑定**：服务器用持久 Ed25519 身份签署 `binding`；部署时应把
  `/v1/server` 的 `public_key` 交给查看者固定（代理的 `--server-pubkey`），
  这样中间人无法伪造“某 ID 属于某公钥”。
- **注册认证**：`--server-key` 保护注册、统计、指标和下线；公钥换绑与指纹
  变更会被拒绝，管理员只能用 `DELETE /v1/identities/{id}` 显式重置。公网部署
  应启用原生 TLS（`--tls-cert`/`--tls-key`）或在前置反向代理终止 TLS，避免
  server-key 与注册签名之外的元数据明文传输。
- **中继与载体认证**：只有用注册公钥通过挑战的主机链路才会被当作主机；UDP
  中继按主机端口绑定，共享载体按 token 选择主机，未认证来源不能在别人的槽位
  收发。查看者令牌只在该主机在线时由查询返回，租约失效后轮换。
- **查询隐私**：离线主机仍返回身份与绑定（供查看者预校验），但不返回候选、
  共享载体与令牌；`/v1/stats` 与 `/metrics` 在配置 `--server-key` 后需要认证。
- **地址审计**：IP 变更只记录注册签名验证通过后的 HTTP 观察地址（或受信
  `--proxy-header`），不采信客户端自报；审计保存在内存中，只经回环管理通道
  读取或清空，清空不重置累计变更计数。
- **滥用限制**：会合端口对 `punch_request` 做按 IP 的固定窗口限速；HTTP API
  有按 IP 的令牌桶（`--api-rate`/`--api-burst`，超限 `429`）；注册另有按 IP
  的速率与不同 ID 数量限制（`--register-*`，超限 `429` + `Retry-After: 1`）；
  中继端口与共享载体在已有活跃查看者时拒绝新来源，并对握手/空闲超时强制
  关闭；所有计数暴露在 `/v1/stats` 与 `/metrics`。管理通道只绑定回环，不
  接受远程来源。
- **运维身份**：`chast-server-identity.key` 与 `chast-server-state.json` 含
  服务器私钥与主机公钥记录，应按服务账号权限保护；丢失身份文件会使既有
  `binding` 全部失效。

## 公网部署：TLS、限流、指标与管理

```sh
chast-server --http-listen 0.0.0.0:8443 --punch-listen 0.0.0.0:8444 \
  --relay-listen 0.0.0.0 --relay-ports 41000-41099 \
  --carrier-listen 0.0.0.0:8445 --advertise 203.0.113.5 \
  --relay-servers 203.0.113.5,203.0.113.6 \
  --server-key "$CHAST_SERVER_KEY" \
  --state /var/lib/chast/server.json --identity /var/lib/chast/identity.key \
  --tls-cert /etc/chast/fullchain.pem --tls-key /etc/chast/privkey.pem \
  --api-rate 50 --api-burst 100 --admin-listen 127.0.0.1:8446 \
  --register-max 30 --register-max-ids 300 --punch-log-max 100000 \
  --relay-single-bandwidth 128 --relay-total-bandwidth 1024 \
  --relay-blacklist-speed 32 \
  --relay-blacklist-file /var/lib/chast/blacklist.txt \
  --relay-blocklist-file /var/lib/chast/blocklist.txt
```

- `--tls-cert` / `--tls-key` 同时给出时 HTTP API 直接以 HTTPS 监听（最低
  TLS 1.2），证书在启动时加载，文件错误直接退出；只给一个视为配置错误。
  都不给则保持明文，适合已有反向代理终止 TLS 的部署。`/v1/server` 的
  `tls` 字段反映当前模式。代理的 `--server https://…` 无需其他改动。
- `--relay-servers`（空值回退 `--advertise`）列出本实例可达的中继入口，
  每项是主机名或 IP，不带端口；注册时轮询选中主入口，并以 `relays` 下发
  全部端点。多网卡或 NAT 多映射时，每个地址都必须路由到同一实例，且中继
  端口能按该地址绑定（也可用解析到本机的 DNS 名）；`rs`/`tg` 用于运行时
  查看、替换与预览，`aur` 仍控制是否强制中继。
- `--api-rate`（默认 50 请求/秒）与 `--api-burst`（默认 100）是所有 HTTP
  路径共用的按 IP 令牌桶，`--api-rate 0` 关闭。超限返回 `429` 和
  `Retry-After: 1`，计入 `chast_http_rate_limited_total`。默认按 TCP 对端
  计费；前置可信代理时用 `--proxy-header X-Forwarded-For` 取最后一跳地址，
  **仅在代理会覆盖该头时启用**，否则客户端可伪造来源绕过限流。
- `GET /metrics` 输出 Prometheus 文本格式，配置 `--server-key` 后需要 Bearer
  认证。采集器示例：

```yaml
scrape_configs:
  - job_name: chast-server
    scheme: https
    static_configs:
      - targets: ["rendezvous.example.com:8443"]
    authorization:
      credentials: "<server-key>"
```

- 指标标签不含主机 ID 与客户端地址：`endpoint` 来自路由模式、`code` 为状态
  码，基数受控。`/v1/stats` 保留为 JSON 快照，二者读取同一份注册表与中继
  统计。
- 配置来源按 hbbs 的顺序叠加：命令行显式参数 > `--config` 指定的 INI >
  `--env-file` 指定的 dotenv（默认 `.env`，缺失时跳过，`--env-file ""` 关闭）
  > 环境变量。INI 只读 `key=value`，分节头与 `#`/`;` 注释被忽略；dotenv
  额外接受 `export ` 与成对引号。键名大小写不敏感、`-` 与 `_` 等价，并且
  优先匹配 `CHAST_<KEY>` 再匹配 `<KEY>`，例如 `CHAST_HOST_TTL=90s`、
  `relay-single-bandwidth=64`。布尔参数也接受 `Y/N`。坏值在启动时报
  `config <flag>=<value>: …` 并退出，不会带病运行。`chast-relay` 使用同一套
  来源与优先级。

### 独立中继节点（chast-relay）

中继负载需要离开 hbbs 所在机器时，部署一个或多个 `chast-relay`：

```sh
chast-relay --http-listen 0.0.0.0:8447 --relay-listen 0.0.0.0 \
  --relay-ports 41000-41099 --carrier-listen 0.0.0.0:8448 \
  --advertise 203.0.113.7 --key "$CHAST_RELAY_NODE_KEY" \
  --node-id relay-1 --admin-listen 127.0.0.1:8449 \
  --relay-single-bandwidth 128 --relay-total-bandwidth 1024 \
  --relay-blacklist-speed 32 \
  --tls-cert /etc/chast/relay-fullchain.pem --tls-key /etc/chast/relay-privkey.pem
```

- 节点同样接受全部中继策略参数与名单文件，策略在节点本地执行，不改变控制面
  或数据面协议。`--admin-listen`（默认 `127.0.0.1:8449`，空值禁用）提供与
  hbbs 相同的一行命令控制台（`stats`、`hosts` 摘要、`sb/tb/ls/dt/t`、
  `ba/br/b`、`Ba/Br/B`、`u`），只绑定回环。

- `--key` 必填，与 hbbs 的 `--relay-node-key` 相同；控制面 `/v1/leases`、
  `/v1/leases/{id}`、`/v1/stats` 都要求 Bearer key，`/healthz` 公开。
  `--tls-cert`/`--tls-key` 让控制面直接走 HTTPS，也可放在反向代理后。
- `--relay-ports` 是节点预绑定的 UDP/TCP 端口集合，端口在租约申请后按
  `--advertise` 解析出的本机地址重绑；`--carrier-listen` 非空时节点提供共享
  载体并在租约响应里返回其地址。`--lease-janitor` 控制到期租约的清理周期。
- 节点只保存 hbbs 下发的公钥与 `carrier_token`，`relay_hello`/`relay_auth`
  与载体握手都在节点本地校验，不需要回连 hbbs。响应里的 `relay`、
  `carrier` 必须从主机和查看者都可达，否则它们仍会拨到节点但连接失败。
- 节点租约 TTL 由 hbbs 传 `2 × --host-ttl`，节点接受 5 s–1 h；hbbs 短暂
  不可用时主机不会立刻失去中继槽位。

hbbs 侧：

```sh
chast-server ... \
  --relay-nodes http://relay-1:8447,https://relay-2:8447 \
  --relay-node-key "$CHAST_RELAY_NODE_KEY" \
  --relay-node-health 10s
```

- 注册时先向该主机上次使用的节点续租，保持端口和查看者不变；首选节点不可用
  时按轮询尝试其余健康节点；全部失败则记录警告并回退本机
  `--relay-servers`/`--carrier-listen`。切换节点或回退时异步释放旧租约。
- 健康探测每 `--relay-node-health`（默认 10 s）请求 `/healthz`，缓存 RTT、
  租约数和共享载体已连接的主机列表。节点失效后 `rs` 显示 `status=down` 与
  最近错误；已有租约要等主机下一次注册刷新才会迁移，已有查看者不迁移。
- `/v1/server` 与 `/v1/stats` 增加 `relay_nodes`（节点 URL、健康、租约、
  载体链接数、RTT）；`/metrics` 增加 `chast_relay_nodes` 与
  `chast_relay_nodes_healthy`。管理通道 `hosts` 增加 `node=`，`rs` 先列
  节点再列本机地址，`tg` 优先返回下一个健康节点。
- 中继治理：`--relay-single-bandwidth`（默认 128）、`--relay-total-bandwidth`
  （默认 1024）、`--relay-blacklist-speed`（默认 32）都以 Mbit/s 计，`0`
  表示该维度不限；`--relay-downgrade-threshold`（默认 0.66）与
  `--relay-downgrade-after`（默认 30m）控制持续高带宽会话的自动降级。
  名单文件可选：`--relay-blacklist-file` 命中后限速，
  `--relay-blocklist-file` 命中后拒绝新会话并断开已有会话；每行取第一个字段，
  支持 `ip note` 写法。未给路径时名单只在内存中，控制台增删后原子回写。
- 节点控制协议：

  | 方法 | 路径 | 认证 | 说明 |
  | --- | --- | --- | --- |
  | `POST` | `/v1/leases` | Bearer key | `{"id","public_key","carrier_token","ttl_sec"}`，幂等续约 |
  | `DELETE` | `/v1/leases/{id}` | Bearer key | 释放租约、端口与链路 |
  | `GET` | `/healthz` | 无 | `{"ok","node","leases","linked"}` |
  | `GET` | `/v1/stats` | Bearer key | 租约、UDP/TCP 端口与共享载体计数 |

### 管理通道、审计与运行时开关

```sh
chast-server ... --admin-listen 127.0.0.1:8446 \
  --register-max 30 --register-window 60s \
  --register-max-ids 300 --register-id-window 24h \
  --punch-log-max 100000
```

- `--admin-listen`（默认 `127.0.0.1:8446`，空值禁用）只接受回环绑定，启动
  时校验监听地址，连接来源不是回环直接关闭。协议与 hbbs 一致：一行明文
  命令（≤1 KiB），处理完写回纯文本并关闭。默认命令如下：

  | 命令 | 说明 |
  | --- | --- |
  | `h` | 命令帮助 |
  | `stats` | 主机/身份、中继端口与流量、阻断、打洞日志、IP 变更审计、中继地址与节点健康数 |
  | `hosts [count]` | 当前租约（默认 20），含在线、候选、中继端口与 `relay_host`、`node`、载体链路、最近 keepalive |
  | `punch-requests`/`pr [start] [count]`、`pr -` | 分页查看/清空打洞日志 |
  | `ip-blocker`/`ib`、`ib <ip>`、`ib -` | 阻断表摘要、单 IP 计数、清空跟踪 |
  | `ip-changes`/`ic [id\|start] [count]`、`ic <id> -`、`ic -` | 分页查看、单主机详情/删除或清空 IP 变更审计 |
  | `relay-servers`/`rs [a,b \| -]` | 查看节点状态与本机地址列表，替换或重置本机列表（仅影响后续分配） |
  | `test-geo`/`tg <ip1> [ip2]` | 校验 IP 参数并预览下一次分配：优先下一个健康节点，否则本机轮换地址 |
  | `relay-only`/`aur [Y\|N]` | 查看或切换强制中继（即时生效，无需重启） |
  | `blacklist-add`/`ba <ip[ip...]>`、`blacklist-remove`/`br <ip\|all>`、`blacklist`/`b [ip]` | 黑名单：命中地址按 `--relay-blacklist-speed` 限速；`b` 列出或查询 |
  | `blocklist-add`/`Ba <ip[ip...]>`、`blocklist-remove`/`Br <ip\|all>`、`blocklist`/`B [ip]` | 阻断名单：命中地址的新会话被拒，已有会话在 1 s 内断开 |
  | `single-bandwidth`/`sb`、`total-bandwidth`/`tb`、`limit-speed`/`ls` | 查看或设置单会话/总带宽/黑名单速度（Mbit/s，`0` 不限） |
  | `downgrade-threshold`/`dt`、`downgrade-start-check`/`t` | 查看或设置降级阈值（0–1）与启动检查时间（秒） |
  | `usage`/`u` | 每个活动会话的 IP、键、时长、字节、峰值/平均/当前速率、降级与黑名单标记 |

- 注册阻断在 server-key、JSON 与 ID 校验之后、公钥与签名验证之前执行，
  因此坏公钥、坏签名的洪泛同样计入。默认同一 IP 每 60 s 最多 30 次尝试，
  第 31 次返回 `429`；同一 IP 每 24 h 最多 300 个不同 ID，超过后只拒绝新
  ID，已见 ID 仍可续约，避免共用出口 NAT 的既有主机被锁死。两个上限设为
  `0` 可分别关闭。
- 打洞日志只在目标在线、会合服务准备回复双方时记录
  `time/from_ip/to_ip/id`；同一 `from_ip + to_id` 在 60 s 内只记一条
  （只扫描最近 30 条）。`--punch-log-max 0` 关闭记录，写满后覆盖最旧条目。
- IP 变更审计对齐 hbbs 的 `IP_CHANGES`：每个 ID 的首次注册观察只建立基线；
  180 s 窗口内出现第二个地址时以“旧地址 + 新地址”建立条目，窗口滚动后只
  保留新地址，条目在 360 s 后剪枝。单地址条目不显示也不计数，但保留在内存
  中，避免自动指标抓取把进行中的审计清空。`ic` 输出
  `id age=… ips=ip:count,…`；`ic <id> -` 同时删除基线与条目（下次观察重新
  建基线），`ic -` 清空全部但保留累计 `changes`。
- 中继地址列表：`rs` 打印当前列表，`rs a,b` 立即替换（只影响新租约，已有
  租约保留 `relay_host`，直到地址被移除后的下一次续约），`rs -` 恢复启动时
  的列表；带端口、URL 或路径的项会被拒绝。配置 `--relay-nodes` 后 `rs` 先
  输出每个节点的 `status`/`leases`/`linked`/`rtt_ms` 与可选 `error`，再列本机
  地址。`tg` 与 hbbs 的 `test-geo` 形式一致，优先返回下一个健康节点的
  `next=<node URL>`（不消耗节点轮换），没有健康节点时退回本机地址并消耗一次
  轮换。
- 计数：`/v1/stats` 的 `registration_blocked`、`punch_requests`、
  `ip_changes`（`tracked`/`changes`）、`relay_servers` 与 `relay_nodes`；
  `/metrics` 的 `chast_register_blocked_total{reason}`、
  `chast_registration_tracked_ips`、`chast_punch_requests_total`、
  `chast_punch_requests_deduped_total`、`chast_punch_log_entries`、
  `chast_ip_change_events_total`、`chast_ip_change_tracked`、
  `chast_relay_servers`、`chast_relay_nodes` 与
  `chast_relay_nodes_healthy`；中继策略另见
  `chast_relay_single_bandwidth_bps`、`chast_relay_total_bandwidth_bps`、
  `chast_relay_blacklist_speed_bps`、`chast_relay_policy_blacklist`、
  `chast_relay_policy_blocklist`、`chast_relay_policy_sessions`、
  `chast_relay_limited_bytes_total`、
  `chast_relay_rate_dropped_total{unit}`、
  `chast_relay_blocked_sessions_total`、
  `chast_relay_downgraded_sessions_total` 与
  `chast_carrier_blocked_total`。

## 目录

```
server/
  cmd/chast-server/      服务端（HTTP + 会合 + 中继 + 共享载体）
  cmd/chast-relay/       独立中继节点（hbbr 等价物：控制面 + UDP/TCP 中继 + 载体）
  cmd/chast-tunnel/      主机/查看者隧道代理
  internal/identity/     Ed25519 身份、挑战消息与服务器绑定
  internal/wire/         控制报文编解码
  internal/registry/     主机注册表、永久身份、租约与中继端口分配
  internal/rendezvous/   UDP 会合、打洞协调、限速
  internal/relay/        UDP 中继与 TCP/WebSocket 载体
  internal/relaynode/    节点租约、本地挑战与健康/统计
  internal/relaypool/    hbbs 侧节点 URL、健康探测、轮换与粘性租约
  internal/carrier/      载体帧编解码与 WebSocket 升级
  internal/api/          HTTP API
  internal/agent/        代理的注册、保活、路径选择与转发
  internal/admin/        回环明文管理通道
  internal/abuse/        注册速率与 ID 洪泛阻断
  internal/punchlog/     打洞请求环形审计日志
  internal/ipchange/     公网地址变更审计（hbbs IP_CHANGES）
  internal/relayaddr/    中继地址列表、轮换与粘性分配
```

## 构建与运行

```sh
cd server
go build ./cmd/chast-server ./cmd/chast-relay ./cmd/chast-tunnel
go test ./...
```

本机验证（服务器、主机代理、查看者代理和 chast 都在同一台机器）：

```sh
chast-server --http-listen 127.0.0.1:8443 --punch-listen 127.0.0.1:8444 \
  --relay-listen 127.0.0.1 --relay-ports 41000-41009 \
  --carrier-listen 127.0.0.1:8445 \
  --advertise 127.0.0.1 --server-key dev-secret \
  --state chast-server-state.json --identity chast-server-identity.key

# 主机：--local 指向 chast-host 的 QUIC 监听端口；
# --carrier 指向 chast-host 的 TCP 载体监听（P+1），没有则禁用该回退
chast-tunnel host --server http://127.0.0.1:8443 --server-key dev-secret \
  --id devbox --key host-secret --local 127.0.0.1:4433 \
  --carrier 127.0.0.1:4434 --listen 127.0.0.1:14434

# 查看者：本地 QUIC 客户端连接 127.0.0.1:24434
chast-tunnel viewer --server http://127.0.0.1:8443 --id devbox \
  --listen 127.0.0.1:24434 --mode auto

# 查看者客户端（令牌来自 chast-host 的输出）
chast-quic connect --host 127.0.0.1 --port 24434 --token <token>
```

`--mode relay` 强制中继，`--mode direct` 只走打洞直连，`--mode carrier` 强制
共享载体，用于分别验证各条路径。服务器加 `--relay-only` 后不再下发直连候选，
效果类似 RustDesk 的 `ALWAYS_USE_RELAY`；`--state ""` 与 `--identity ""` 用于
一次性测试；`--host-ttl` 和 `--offline-after` 控制租约与在线判定；
`--carrier-listen ""` 关闭共享载体。`--relay-servers` 在本机回环可省略
（默认取 `--advertise`）；验证地址轮换时加 `--relay-servers
127.0.0.1,127.0.0.2`，再用管理通道 `rs`/`tg` 查看分配。IP 变更审计默认
启用（180 s 窗口、360 s 保留，上限 65536 个 ID）。
`chast-tunnel host --help` / `viewer --help` 列出全部参数。

## 查看者集成（Flutter）

M4.1 起 Flutter 查看者不再要求手工启动查看者代理：会话页启动随包的
`chast-tunnel.exe viewer`，代理监听 `127.0.0.1:0`，把系统分配的 UDP 端口经
stdout 事件交回，`ViewerEngine` 再连接该端口。代理默认从查看者可执行文件
同目录查找，可用 `CHAST_TUNNEL` 环境变量指定其他路径。

stdout 是面向嵌入方的 JSON Lines 事件流（诊断仍在 stderr）：

```json
{"event":"ready","listen":"127.0.0.1:52144","host":"devbox","path":"connecting","direct":"198.51.100.7:51234","relay":"203.0.113.5:41007","carrier":"203.0.113.5:8445","carrier_token":"<32 hex>"}
{"event":"path","path":"direct","remote":"198.51.100.7:51234","direct":"198.51.100.7:51234","relay":"203.0.113.5:41007"}
{"event":"path","path":"relay","remote":"203.0.113.5:41007","direct":"198.51.100.7:51234","relay":"203.0.113.5:41007","carrier":"203.0.113.5:8445","carrier_token":"<32 hex>"}
```

`ready` 表示本地监听已就绪，`path` 在首次选定路径或 `auto` 回退到中继时输出。
`carrier`/`carrier_token` 由认证查询取得；会话页在二者存在时优先把共享载体
交给载体桥，否则回退到中继端口的 TCP 载体或直连的 `host:(P+1)`。
证书指纹 pin 使用逻辑身份 `server:<url>/<id>`，不是代理的 loopback 端口。

## 主机集成（托盘）

M4.2 起 `chast-host.exe`（Win32 托盘）自己托管主机代理：HostServer 成功监听后
启动随包的 `chast-tunnel.exe host`，停止共享、重载设置或退出托盘时先停代理，
因此服务端不会在主机停服后仍显示在线。代理默认从 `chast-host.exe` 同目录查找，
可用 `CHAST_TUNNEL` 覆盖。

连接服务配置保存在 `host.ini`：

```ini
tunnel=1
server_url=https://rendezvous.example.com
server_key=...
tunnel_id=devbox
tunnel_key=...
tunnel_listen=0.0.0.0:14434
carrier=1
```

- `server_url` 为空表示不注册（仅局域网直连）；`tunnel=0` 时不启动代理。
- `tunnel_id`/`tunnel_key` 首次启动按机器名自动生成（密钥 16 字符，可修改）；
  `tunnel_key` 派生 Ed25519 私钥用于注册与挑战签名，从不上传，服务端只保存
  公钥和 SHA-256 派生后的身份绑定。
- `tunnel_listen` 必须与主机 QUIC 的 `port` 不同（两者都是 UDP 监听），默认
  `0.0.0.0:14434`。`carrier=1`（默认）让 HostServer 在 `port+1` 上监听 TCP/WS
  载体；托盘“允许防火墙”为主机端口与隧道端口各建一条按程序限定的 UDP 入站
  规则，并加一条载体 TCP 规则；禁用隧道服务时会删除这些规则。

主机代理的 stdout 同样只输出 JSON Lines 机器事件（字段随事件可选）：

```json
{"event":"ready","listen":"127.0.0.1:14434","host":"devbox","punch":"198.51.100.7:51234","relay":"203.0.113.5:41007"}
{"event":"relay","listen":"127.0.0.1:14434","host":"devbox","punch":"198.51.100.7:51234","relay":"203.0.113.5:41007"}
{"event":"viewer","listen":"127.0.0.1:14434","host":"devbox","path":"direct","remote":"198.51.100.7:51234","relay":"203.0.113.5:41007"}
```

`ready` 在注册与中继绑定完成后输出；`relay` 在重新注册后中继端点变化时输出；
`viewer` 在查看者首次出现或路径变化时输出。HostServer 看到的对端永远是代理的
loopback 地址，所以路径由代理按数据报来源判定（来源等于分配到的中继端点即
`relay`），托盘只在 HostServer 正在串流时展示“最近路径”。

## 验证记录

2026-10-21，在单机回环上跑通完整链路：`chast-server` + `chast-tunnel host` +
`chast-tunnel viewer` + `chast-quic`（DXGI 采集 → oneVPL 编码 → QUIC → D3D11VA
解码），查看者断言端到端帧数与摘要。服务端使用 `--server-key dev-secret`，
主机 `--id devbox --key host-secret`，令牌由 `serve --token auto --token-reuse`
生成。

- 直连（`viewer --mode direct`）：`sender: frames=201 encoded=201 ... peer
  received=201 decoded=201 digest=E198527DF2B2B548`，退出码 0；同一时刻中继
  统计为 `to_host_bytes:0, to_viewer_bytes:0`，说明数据没有经过中继。
- 中继（`viewer --mode relay`）：`frames=202 ... received=202 decoded=202
  digest=90C686753B88E5A2`，退出码 0；中继端口 41000 统计为
  `to_host_bytes:16402, to_viewer_bytes:2501405`，`viewer` 为查看者代理地址。
- HTTP：`GET /v1/hosts/devbox` 返回 `online:true`、主机候选地址、中继地址
  `127.0.0.1:41000` 和会合地址 `127.0.0.1:8444`；查看者代理正是用这组信息
  完成打洞与回退。
- 自动化：`go test ./...` 覆盖控制报文、注册表、HTTP API、会合与中继以及
  `internal/agent` 的三种模式；其中 agent 测试用真实 UDP 回显和进程内服务器，
  分别断言直连路径不经过中继、强制中继路径双向转发、`auto` 在直连失败后回退，
  以及未知主机、错误密钥、第二查看者 `busy` 等失败路径。

2026-10-22，M4.1 的 Flutter 集成在同样的单机回环上复测：Dart `TunnelAgent`
启动 `chast-tunnel viewer --listen 127.0.0.1:0`，`chast-quic connect` 连接
`ready` 事件给出的本地端口。直连模式 `received=201 decoded=201 digest=A188ABD0B85F082B`，
中继计数全 0；强制中继 `received=202 decoded=202 digest=E092051892076F56`，
中继统计 `to_host_bytes:18054, to_viewer_bytes:2575942`。两次均无 CRC、解码、
序号或协议错误，代理报告的最终路径分别为 `direct` 与 `relay`。

2026-10-22，M4.2 的托盘主机集成在单机回环上验证：`chast-host --tray` 读取
`host.ini` 后自动启动同目录 `chast-tunnel host`，`GET /v1/hosts/m42-host` 返回
`online=true`、候选 `127.0.0.1:16434`、中继 `127.0.0.1:41100`、会合观察地址
`127.0.0.1:18444` 与指纹 `C2F256…6421`；托盘显示“连接服务：已在线 · 中继
127.0.0.1:41100 · 设备 ID m42-host”。

- 直连：`chast-tunnel viewer --mode direct` 输出 `path=direct`，
  `chast-quic connect` 得到 `received=603 decoded=603 keyframes=1 crc-errors=0
  decode-errors=0 gaps=0 digest=610B8EF3346AD47C`；托盘显示“最近路径：直连 ·
  127.0.0.1:24434”，同期中继计数为 0。
- 中继：`--mode relay` 输出 `path=relay, remote=127.0.0.1:41100`，
  `received=603 decoded=603 keyframes=1 crc-errors=0 decode-errors=0 gaps=1
  digest=659B01211DD715C6`；首帧序号 1708 是中途加入已在运行的编码流，不是丢帧。
  托盘显示“最近路径：经中继 · 127.0.0.1:41100”；`/v1/stats` 的 41100 端口为
  `to_host_bytes:35677, to_viewer_bytes:10594896, dropped:0`。
- 清理：`WM_COMMAND` 退出托盘后 `chast-host`/`chast-tunnel` 进程均消失；注册在
  TTL（约 50 s）后从 API 消失（`host is not registered`）。
- 防火墙：`chast-host.exe --firewall 46333 16434` 实测生成两条按程序限定的 UDP
  入站规则（`chast host`/`chast tunnel`），验证后删除。
- 自动化：`go test ./...` 覆盖主机代理的 `ready`、直连/中继分类与切换去重；
  `ctest -C Release` 5/5 通过，其中 `host_tunnel` 覆盖事件解析、参数引用与子进程
  生命周期。

2026-10-24，服务端对齐阶段（持久身份、挑战应答、共享载体）在单机回环上验证：

- 共享载体 TCP：`chast-server --carrier-listen 127.0.0.1:8445`，主机代理建立
  `CHASTT-HOST2` 链路后，`chast-quic connect --transport tcp --carrier
  127.0.0.1:8445 --carrier-id devbox --carrier-token <lookup token>` 得到
  `received=692 decoded=692 keyframes=1 crc-errors=0 decode-errors=0 gaps=0
  digest=B2A69C6EA526D137`，`transport=tcp`；桥统计 out=4126/in=10562 个数据报。
- 共享载体 WebSocket：同样 692 帧、摘要 `EB0FC78F17BE208E`，`transport=ws`；
  桥统计 out=4274/in=10595。
- 负向：错误 `carrier_token` 被拒绝；缺少 token 的桥在构造时报错；
  `/v1/stats` 的 `relay.shared_carrier` 显示
  `{"addr":"127.0.0.1:8445","hosts":0,"host_links":10,"viewers":0,"busy":0,
  "rejected":1,"to_host_bytes":401418,"to_viewer_bytes":23022144}`。
- 自动化：`go vet ./...`、`go build ./...`、`go test ./...` 通过；
  `chast-carrier-test` PASS（共享 hello 魔数、host id + token、缺 token 拒绝）；
  `ctest -C Release` 20/20；`flutter analyze` 无问题、`flutter test` 106 项通过。
- 身份：注册签名、公钥换绑拒绝、离线查询返回绑定、keepalive/relay 挑战的
  一次性与过期、共享载体主机认证与令牌轮换由 Go 单测覆盖；服务器身份的
  `binding` 可用 `/v1/server` 的 `public_key` 独立验证。

2026-10-25，运维能力（原生 TLS、HTTP 限流、Prometheus 指标）在单机回环上验证：

- 明文：`--api-rate 2 --api-burst 3` 下连续 `/healthz` 得到
  `200,200,429,429`，1.2 s 后恢复 `200`；`/v1/server` 返回 `"tls":false`；
  `/metrics` 含 `chast_server_info{...tls="false"} 1`、`chast_hosts`、中继/
  载体与 `chast_http_requests_total` 计数，未带 server-key 时为 401。
- HTTPS：自签证书（SAN `IP:127.0.0.1`）下用 `--tls-cert`/`--tls-key` 启动，
  `curl --cacert cert.pem https://127.0.0.1:18446/healthz` 返回 200，
  `/v1/server` 带 `"tls":true`；`openssl s_client` 分别协商 TLSv1.3 与
  TLSv1.2 且证书校验通过；同一端口收到明文 HTTP 请求时返回 Go 标准 400
  `Client sent an HTTP request to an HTTPS server.`。
- 自动化：`go vet ./...`、`go build ./...`、`go test ./...` 通过；
  `internal/ratelimit` 覆盖突发/补充/隔离/回收，`internal/api` 覆盖 429、
  `Retry-After`、代理头取址、指标内容与鉴权，`cmd/chast-server` 用真实 TLS
  监听完成 HTTPS 探测并断言最低 TLS 1.2。

2026-10-25，载体在线、强制中继与查看者回退（M10.3）在单机回环上验证：

- 服务器 `--relay-only --offline-after 5s`，主机 `devbox` 带本地载体、
  `relaybox` 不带载体、keepalive 300 ms；`GET /v1/hosts/devbox` 返回
  `online:true`、`relay_only:true`、`nat_type:"symmetric"`、
  `transport:"carrier"`、有效 `carrier_token`，且没有
  `candidate`/`candidates`/`local_addrs`；`relaybox` 返回
  `transport:"relay"`。`chast-tunnel viewer --mode carrier` 监听
  `127.0.0.1:19200`，UDP 回环数据报往返成功；`--mode auto` 对 `relaybox`
  走 UDP 中继并往返成功；`--mode direct` 以
  `server forces relay; direct mode is unavailable` 退出（exit 1）。
- 仅载体在线：服务器 `--offline-after 1s`，主机 keepalive 5 s，等待 3 s 后
  `last_seen_ms` 已远超离线窗口，查询仍返回 `online:true`、
  `transport:"carrier"` 和令牌，且无直连候选；同一主机经载体完成数据报往返。
- 自动化：`go vet ./...`、`go build ./...`、`go test ./...` 通过；
  `internal/relay` 新增 `HostLinked` 翻转与查看者保活应答测试；
  `internal/api` 覆盖 `relay_only`/`transport`；`internal/agent` 覆盖载体模式
  往返与断链重连、UDP 停更后经载体在线、强制中继下 `auto` 选中继与 `direct`
  报错。C++/Flutter 未改动，不重跑全量。

2026-10-25，管理通道、打洞审计与注册防滥用（M10.4）在单机回环上验证：

- 服务器 `--admin-listen 127.0.0.1:18446 --offline-after 5s --register-max 3
  --register-window 60s --register-max-ids 5 --punch-log-max 100`，主机
  `devbox` keepalive 2 s，查看者 `--mode auto`。管理通道 `stats` 显示
  `hosts=1`、`punch_log stored=1 total=1`；`hosts` 显示 `devbox` 在线、
  候选 `127.0.0.1:19434`；`pr` 输出
  `2026-10-24T16:33:12Z 127.0.0.1 -> devbox@127.0.0.1`；`pr 0 1` 分页
  正常，`pr -` 返回 `cleared`，重启查看者后新打洞再次入账（清空后
  `stored=1 total=2`，说明清空保留累计计数）。
- `aur Y` 后 `/v1/hosts/devbox` 返回 `relay_only:true`、
  `transport:"relay"`、无 `candidate`/`candidates`，`nat_type:"symmetric"`；
  `/v1/server` 同步 `relay_only:true`；`aur N` 后恢复 `transport:"direct"`
  与候选，无需重启。
- `regflood -n 4` 对同一 IP 得到 `200,200,200,429`；`ib 127.0.0.1` 显示
  `count=4 ids=3`、摘要 `rate_blocked=1`；`/v1/stats` 的
  `registration_blocked` 为
  `{"tracked_ips":1,"rate_blocked":1,"id_blocked":0}`，`/metrics` 的
  `chast_register_blocked_total{reason="rate"} 1` 与之一致；`ib -` 返回
  `cleared 1`。
- 负向：`--admin-listen 0.0.0.0:18456` 启动即失败
  `admin listen "0.0.0.0:18456" must be a loopback address`；非回环来源
  由 `internal/admin` 测试覆盖。
- 自动化：`go vet ./...`、`go build ./...`、`go test ./... -count=1` 通过；
  `internal/punchlog` 覆盖顺序/分页/去重/容量/清空，`internal/abuse` 覆盖
  窗口滚动、ID 洪泛与淘汰，`internal/admin` 覆盖命令与来源校验，
  `internal/api` 覆盖 429、已见 ID 续约与原子 `relay_only`，
  `internal/rendezvous` 覆盖打洞入账与目标离线不入账。

2026-10-25，IP 变更审计与中继地址轮换（M10.5）在单机回环上验证：服务器
`--relay-listen 0.0.0.0 --relay-servers 127.0.0.1,127.0.0.2`，主机
devbox/devbox2/devbox3、`--mode relay` 查看者与 UDP 回显目标。

- 轮换与粘性：devbox 注册得到 `127.0.0.1:41100`，devbox2 得到
  `127.0.0.2:41101`；两者注册响应都带两个端点的 `relays`，主机日志为
  `relay challenge set updated endpoints="[127.0.0.1:41100 127.0.0.2:41100]"`；
  两个主机的中继查看者都完成 UDP 回显往返。
- 源地址：`netstat` 显示端口按租约绑定到分配地址
  （`UDP/TCP 127.0.0.1:41100`、`127.0.0.2:41101`）。修复前通配监听按路由
  选择源地址，访问 `127.0.0.2:41101` 的查看者收到来自 `127.0.0.1:41101`
  的回包并丢弃；`relay.Manager.BindHost` 在注册时重绑 UDP/TCP 监听后回包
  源地址与目标一致。devbox 从 `127.0.0.4` 重启注册仍保持
  `127.0.0.1:41100`，再次探测正常。
- IP 审计：devbox 在重启加入 `127.0.0.4` 后为
  `ips=127.0.0.1:4,127.0.0.2:2,127.0.0.4:2`，devbox2 为
  `127.0.0.1, 127.0.0.3`。回环下 HTTP 注册源固定为 `127.0.0.1`、keepalive
  源是主机绑定地址，因此同一主机在两个地址间交替计数；生产环境两条通路
  看到同一个公网地址，不会产生这种抖动。
- 管理通道：`rs` 列出两个地址；`rs 127.0.0.2` 后已有 devbox/devbox2 的
  `relay_host` 保持不变，新注册的 devbox3 得到 `127.0.0.2:41102` 且中继
  回显正常；`rs -` 恢复两地址；`rs bad:123` 被拒绝
  （`must not include a port`）；`tg 192.0.2.10 192.0.2.11` 返回
  `next=127.0.0.1` 并消耗一次轮换；`ic 0 1` 分页、`ic devbox2 -` 删除、
  `ic -` 清空（`cleared 2`，`changes` 保持 15）。
- 指标与接口：`/metrics` 输出 `chast_ip_change_events_total 14`、
  `chast_ip_change_tracked 3`、`chast_relay_servers 2`；`/v1/stats` 的
  `ip_changes` 与 `relay_servers` 与之一致；`/v1/server` 的
  `relay_servers`、查询响应的 `relay`、`hosts` 的 `relay_host` 均与分配
  一致。
- 自动化：`go vet ./...`、`go build ./...`、`go test ./... -count=1` 通过；
  `internal/ipchange` 覆盖基线/窗口/剪枝/分页/删除/清空，
  `internal/relayaddr` 覆盖规范化、轮询、粘性续约、替换与重置，
  `internal/relay` 覆盖 `BindHost` 回包源地址，`internal/admin` 与
  `internal/api` 覆盖新命令、新字段与注册轮换。

2026-10-25，独立中继节点（M10.6）在单机回环上验证：

- 拓扑：`chast-relay`（控制面 `127.0.0.1:18447`、端口 `41100-41199`、载体
  `127.0.0.1:18448`、`--advertise 127.0.0.1`）与 `chast-server`
  （`--relay-nodes http://127.0.0.1:18447 --relay-node-key … --relay-node-health
  1s --host-ttl 30s`，本机中继 `41200-41299` 作为回退），主机/查看者代理与
  UDP 回显目标；协议仍 v23，代理未改动。
- 远程租约：devbox 得到 `relay=127.0.0.1:41100`、`carrier=127.0.0.1:18448`；
  节点 `/healthz` 返回 `leases:1`；`/v1/server`、`/v1/stats` 的 `relay_nodes`
  与 `/metrics` 的 `chast_relay_nodes 1`、`chast_relay_nodes_healthy 1` 一致。
- 数据面：`viewer --mode relay` 经节点往返三次成功，节点该端口统计
  `to_host_bytes:51, to_viewer_bytes:51`，hbbs 本机中继端口计数为 0；
  `hosts` 显示 `node=http://127.0.0.1:18447`，`rs` 为
  `status=up leases=1 linked=0`，`tg` 返回 `next=http://127.0.0.1:18447`。
- 故障与回退：停止节点后 `rs` 变为 `status=down` 并带连接拒绝错误；主机下次
  续租回退本机中继（`relay=127.0.0.1:41200`、`node=-`、`assigned=1`），本地
  中继回显三次成功。
- 恢复：重启节点后主机续租迁回 `127.0.0.1:41100`，本机端口释放
  （`assigned=0`），新查看者经节点回显三次成功。
- 共享载体：本地载体桥就绪后节点 `/healthz` 返回 `linked:["devbox"]`，
  `hosts` 显示 `carrier=true`；`viewer --mode carrier` 建立
  `carrier 127.0.0.1:18448` 路径并完成 UDP 回显，节点 `/v1/stats` 的
  `shared_carrier` 为 `hosts:1, viewers:1, to_host_bytes:45,
  to_viewer_bytes:45`；连续 10 次 carrier viewer 启动均成功。
- 自动化：`gofmt -l .` 无输出，`go vet ./...`、`go build ./...`、
  `go test ./... -count=1` 通过。

2026-10-25，中继带宽限制、黑名单与降级（M10.7）在单机回环上验证：

- 拓扑：`chast-relay`（控制面 `127.0.0.1:18547`、端口 `41300-41399`、载体
  `127.0.0.1:18548`、控制台 `127.0.0.1:18549`）与 `chast-server`（API
  `127.0.0.1:18543`、管理台 `127.0.0.1:18546`、本机中继 `41400-41499`，
  `--relay-nodes http://127.0.0.1:18547`），主机/查看者代理与 UDP 回显目标；
  协议仍 v23，代理未改动。
- 参数与呈现：节点 `--relay-single-bandwidth 0.08 --relay-total-bandwidth 100
  --relay-blacklist-speed 0.05`，`dt 0.01`/`t 3` 后 `stats` 为
  `single_bps=83886 total_bps=104857600 blacklist_bps=52428 ratio=0.01
  after=3s`；hbss 以 `64/512/16` 启动，`/v1/stats` 的 `relay.policy` 与
  `/metrics` 的 `chast_relay_single_bandwidth_bps` 等一致。节点与 hbss 的
  `h/stats/sb/tb/ls/dt/t/b/B/u`、`ba/br/Ba/Br/all` 均可用，文件回写生效。
- 阻断：会话前 `Ba 127.0.0.1` 使查看者探针无回包（`blocked_sessions=1`、
  端口 `dropped=1`），`Br` 后 3 次往返成功（双向各 21 字节）。会话进行中新
  增阻断的查看者槽位释放由集成测试覆盖。
- 限速与降级：`sb 0.08` 下 burst 400 × 1400 B 只收到 4 个回包；节点计数
  `limited_bytes=319200`、`rate_dropped_datagrams=228`、
  `rate_dropped_bytes=319200`、`downgraded_sessions=1`，`u` 为
  `downgraded=true`；端口 `41300` 为 `to_host=58821 to_viewer=5621
  dropped=229 blocked_sessions=1`。
- 运行时名单：`ba 127.0.0.1` 后 `u` 显示 `blacklisted=true`，`br all` 后恢复
  `false`；预置 `seed-blacklist.txt`/`seed-blocklist.txt` 的第二个节点启动后
  `b`/`B` 各含一条，`stats` 为 `blacklist=1 blocklist=1`。
- 故障回退：停止节点后主机续租回退 `127.0.0.1:41400`，新查看者路径重复阻断
  与 burst：`/metrics` 的 `chast_relay_limited_bytes_total 338800`、
  `chast_relay_rate_dropped_total` 为 `242`（`bytes` 338800）、
  `chast_relay_blocked_sessions_total 1`、
  `chast_relay_downgraded_sessions_total 1`，端口 `41400` 为
  `to_host=49021 to_viewer=15421 dropped=243 blocked_sessions=1`。
- 自动化：`gofmt -l .` 无输出，`go vet ./...`、`go build ./...`、
  `go test ./... -count=1` 通过；`internal/relay/policy_test.go` 覆盖令牌桶、
  名单、降级、命令与配置，`policy_relay_test.go` 覆盖 UDP 限速、阻断拒绝与
  释放、每端口与共享载体阻断，`internal/admin` 覆盖 `PolicyOnly` 控制台，
  `internal/api` 覆盖策略指标。

环境边界：单机回环只有一张网卡，`symmetric` NAT 分类、局域网候选与真实 IPv6
跨网行为需要双机复核；共享载体尚未经过真实 HTTP 代理/CDN；证书轮换、ACME
与 mTLS 不在本阶段范围。`--relay-servers` 的多地址轮换只在回环别名上验证，
多网卡与 DNS 轮换仍需真实部署复核。M10.6 的独立节点、故障回退与恢复已在单机
回环验证，但真实跨机时延、节点带外故障与多节点负载分配仍需真实部署复核。
M10.7 的令牌桶、名单与降级只在回环的低速率下验证，真实链路上的整形效果、
名单一致性与多节点独立策略仍需部署复核。中继/载体统计中的字节数包含两侧
QUIC 握手与重传，与编码器输出的裸帧大小不可直接比较。
