# chast-server — 会合、打洞与中继

Go 实现的 chast 连接服务：主机注册、查看者寻址、UDP 打洞协调和不可达时的
中继回退。服务端只转发加密的 QUIC 数据报，不解析、不终止 chast 协议，因此
协议 v7 的令牌、审批、证书指纹和输入通道都保持端到端语义。

参考实现是 RustDesk 的 `hbbs`（会合/信令）与 `hbbr`（中继）：

| RustDesk | 本服务 | 说明 |
| --- | --- | --- |
| `RegisterPeer` / `RegisterPk` | `POST /v1/hosts` + UDP keepalive | 主机登记 ID、密钥摘要和地址 |
| `PunchHoleRequest` / `PunchHole` | UDP `punch_request` / `punch_peer` | 交换观察到的公网地址并让双方互发探针 |
| `PunchHoleSent` / `FetchLocalAddr` | 直连探针 + 观察地址 | 双方从同一数据套接字打洞 |
| `RequestRelay`（uuid 配对） | 按主机分配 UDP 中继端口 | 查看者直接连该端口，主机侧代理回连中继 |
| 中继按 uuid 配对两条 TCP 流 | 透明转发 UDP 数据报 | 不需要拆 QUIC 流，握手与加密都在端点上 |

## 设计约束

1. **chast 的传输是 QUIC（MsQuic），视频与控制都在 QUIC 流里**。中继如果按
   TCP 字节流配对，就必须终止 QUIC 或在客户端改造传输层。这里选择 UDP 数据报
   转发：每个主机获得一个专属中继 UDP 端口，查看者把该端口当作普通 QUIC 服务
   端地址；中继把数据报原样送到主机侧代理，由主机侧代理再送到
   `chast-host` 的监听端口。QUIC 连接仍是查看者 ↔ `chast-host`，中继看不到流
   边界，也无需理解 chast。
2. **MsQuic 不暴露内部 UDP 套接字**，无法在 QUIC 自己的套接字上做 STUN/打洞。
   因此 NAT 穿透由两侧的 `chast-tunnel` 代理完成：代理各自持有一个对外 UDP
   套接字，用该套接字同时做保活、探测和真实数据转发。直连时数据不经过服务器；
   中继时服务器只做无状态转发。
3. **同一时刻一个主机只服务一个查看者**（现有 `HostServer` 的语义）。中继端口
   因此只维护一个活跃查看者，另一个来源在活跃期内会被拒绝（`busy`），空闲后
   才可接管，避免两路 QUIC 数据报混在一起。
4. 服务器**从不接触 chast 令牌和证书指纹的明文语义之外的内容**。HTTP 注册时
   可携带指纹用于展示，令牌始终只存在于 QUIC 握手内部。

## 拓扑

```
                HTTP 8443/tcp 注册、查询
  chast-tunnel host  ───────────────────────────┐
        │  UDP 保活 / punch_request              │
        │  UDP 8444 会合                         │   chast-server
        │  UDP HELLO + 数据 ↔ 41000..41099 中继  │
        ▼                                        │
  chast-host :4433 (QUIC)                        │
        ▲                                        │
        │ 直连（打洞成功后，同一数据套接字）     │
        │ 或经中继（主机的出站映射）             │
  chast-tunnel viewer ───────────────────────────┘
        ▲ 127.0.0.1:24434 (UDP)
        │
  chast-quic / Flutter 查看者
```

主机代理：

- 用 `POST /v1/hosts` 注册 `id`、`key`（只上传 SHA-256 摘要）、可选指纹；
- 用数据套接字向 UDP 会合端口发送 keepalive，服务器记录其**观察到的公网地址**；
- 用同一套接字向分配到的中继端口发送 `relay_hello`，服务器记录主机侧中继地址；
- 收到 `punch_peer` 后向查看者候选地址发探针，打开本侧 NAT；
- 收到的查看者数据报（直连来源或中继来源）转发给 `--local` 指定的
  `chast-host`；`chast-host` 的回包发回最后一个远端来源。由于两侧代理都只有
  一个数据套接字，直连切中继时 QUIC 看到的五元组不变。

查看者代理：

- 用 `GET /v1/hosts/{id}` 查询在线状态、主机候选地址、中继地址和会合地址；
- 从自己的数据套接字发 `punch_request`，服务器把查看者观察地址通知主机；
- `auto` 模式先向主机候选地址发真实 QUIC 数据报，同时发打洞探针；
  `--direct-timeout` 内收到主机回包就锁定直连，否则切到中继端口并保持；
- `direct` / `relay` 模式用于强制验证两条路径。

局域网内查看者也可以不经过代理，直接用查询到的候选地址连接 `chast-host`；
代理主要解决跨网直连和中继回退。

## 控制协议

所有控制报文都是 **8 字节魔数 `CHASTC1\n` + UTF-8 JSON**，走 UDP；不以魔数开头
的数据报一律视为不透明数据。控制报文只由代理和服务器处理，绝不转发给
`chast-host`。

| type | 方向 | 字段 | 用途 |
| --- | --- | --- | --- |
| `keepalive` | 主机 → 会合 | `host_id`, `key_hash` | 更新观察地址与在线状态 |
| `keepalive_ack` | 会合 → 主机 | `ok`, `candidate`, `relay`, `error` | 返回观察地址和中继地址 |
| `punch_request` | 查看者 → 会合 | `host_id`, `nonce` | 请求与主机交换候选地址 |
| `punch_peer` | 会合 → 双方 | `nonce`, `candidate`, `relay` | 对方的观察地址；主机收到查看者地址，查看者收到主机地址 |
| `punch` | 代理 ↔ 代理 | `nonce` | 打开 NAT 映射的探针，收到即说明路径可用 |
| `relay_hello` | 主机 → 中继 | `host_id`, `key_hash` | 绑定中继端口与主机侧地址 |
| `relay_hello_ack` | 中继 → 主机 | `ok`, `error` | 认证结果 |
| `busy` | 中继 → 查看者 | `error` | 活跃查看者占用中继端口 |

### HTTP

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| `POST` | `/v1/hosts` | `Authorization: Bearer <server-key>`（配置了才需要） | 注册或续约，返回 `relay`、`punch`、`expires_in_sec` |
| `GET` | `/v1/hosts/{id}` | 无 | 在线状态、候选地址、中继/会合地址、指纹 |
| `DELETE` | `/v1/hosts/{id}` | Bearer server-key | 主机主动下线并释放中继端口 |
| `GET` | `/healthz` | 无 | 存活检查 |
| `GET` | `/v1/stats` | 配置了 server-key 时需要 | 注册数、中继字节数和丢包计数 |

主机 `key` 至少 8 个字符，服务器只保存 SHA-256 摘要；续约时摘要必须一致，
否则拒绝，防止他人用同一 ID 占用或劫持中继端口。

示例：

```json
POST /v1/hosts
{"id":"devbox","key":"correct horse","fingerprint":"AB12..."}
→ 200 {"id":"devbox","relay":"203.0.113.5:41007","punch":"203.0.113.5:8444","expires_in_sec":60}

GET /v1/hosts/devbox
→ 200 {"id":"devbox","online":true,"candidate":"198.51.100.7:51234",
       "relay":"203.0.113.5:41007","punch":"203.0.113.5:8444",
       "fingerprint":"AB12...","last_seen_ms":1200,"expires_in_ms":48000}
```

## NAT 行为与边界

- 公网地址由服务器在 UDP 会合端口观察得到，不信任客户端自报；主机的候选地址
  是其数据套接字到会合端口的映射。
- 双方从**同一个**数据套接字向对方候选地址发探针，打洞对锥形 NAT
  （全锥/受限/端口受限）有效。对称 NAT 下不同目的地的映射端口不同，直连会
  失败，查看者代理在 `--direct-timeout` 后自动切中继；这是有意保留的回退，
  不伪装成"总能直连"。
- 中继端口按主机分配，服务器重启后重新分配；主机代理会在续约时更新地址。
- 中继是 UDP，无拥塞控制；chast 的 QUIC 自身有拥塞控制与重传。中继会对每个
  端口限制活跃查看者数量为 1，并限制控制报文速率，降低开放反射风险。
- QUIC 初始包被填充到至少 1200 字节，中继按原样转发，不放大流量。

## 安全

- **传输安全**：chast 的 QUIC TLS 证书、指纹 pinning、一次性/可复用令牌和
  连接审批完全在端点上完成，服务器或中继无法解密视频与控制消息。
- **注册认证**：`--server-key` 保护注册、统计和下线；主机 `key` 的摘要用于
  续约和 `relay_hello`。公网部署应在 HTTP 前放 TLS 反向代理，避免 server-key
  明文传输。
- **中继认证**：只有用有效主机摘要通过 `relay_hello` 的地址才会被当作主机；
  未认证来源不能改绑端口。查看者数据只转发给该主机。
- **滥用限制**：会合端口对 `punch_request` 做按 IP 的固定窗口限速；中继端口
  在已有活跃查看者时拒绝新来源；所有计数暴露在 `/v1/stats`。

## 目录

```
server/
  cmd/chast-server/      服务端（HTTP + 会合 + 中继）
  cmd/chast-tunnel/      主机/查看者隧道代理
  internal/wire/         控制报文编解码
  internal/registry/     主机注册表与中继端口分配
  internal/rendezvous/   UDP 会合、打洞协调、限速
  internal/relay/        按主机端口的 UDP 中继
  internal/api/          HTTP API
  internal/agent/        代理的注册、保活、路径选择与转发
```

## 构建与运行

```sh
cd server
go build ./cmd/chast-server ./cmd/chast-tunnel
go test ./...
```

本机验证（服务器、主机代理、查看者代理和 chast 都在同一台机器）：

```sh
chast-server --http-listen 127.0.0.1:8443 --punch-listen 127.0.0.1:8444 \
  --relay-listen 127.0.0.1 --relay-ports 41000-41009 \
  --advertise 127.0.0.1 --server-key dev-secret

# 主机：--local 指向 chast-host 的 QUIC 监听端口
chast-tunnel host --server http://127.0.0.1:8443 --server-key dev-secret \
  --id devbox --key host-secret --local 127.0.0.1:4433 --listen 127.0.0.1:14434

# 查看者：本地 QUIC 客户端连接 127.0.0.1:24434
chast-tunnel viewer --server http://127.0.0.1:8443 --id devbox \
  --listen 127.0.0.1:24434 --mode auto

# 查看者客户端（令牌来自 chast-host 的输出）
chast-quic connect 127.0.0.1:24434 --token <token>
```

`--mode relay` 强制中继，`--mode direct` 只走打洞直连，用于分别验证两条路径。
`chast-tunnel host --help` / `viewer --help` 列出全部参数。

## 查看者集成（Flutter）

M4.1 起 Flutter 查看者不再要求手工启动查看者代理：会话页启动随包的
`chast-tunnel.exe viewer`，代理监听 `127.0.0.1:0`，把系统分配的 UDP 端口经
stdout 事件交回，`ViewerEngine` 再连接该端口。代理默认从查看者可执行文件
同目录查找，可用 `CHAST_TUNNEL` 环境变量指定其他路径。

stdout 是面向嵌入方的 JSON Lines 事件流（诊断仍在 stderr）：

```json
{"event":"ready","listen":"127.0.0.1:52144","host":"devbox","path":"connecting","direct":"198.51.100.7:51234","relay":"203.0.113.5:41007"}
{"event":"path","path":"direct","remote":"198.51.100.7:51234","direct":"198.51.100.7:51234","relay":"203.0.113.5:41007"}
{"event":"path","path":"relay","remote":"203.0.113.5:41007","direct":"198.51.100.7:51234","relay":"203.0.113.5:41007"}
```

`ready` 表示本地监听已就绪，`path` 在首次选定路径或 `auto` 回退到中继时输出。
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
```

- `server_url` 为空表示不注册（仅局域网直连）；`tunnel=0` 时不启动代理。
- `tunnel_id`/`tunnel_key` 首次启动按机器名自动生成（密钥 16 字符，可修改）；
  密钥不上传，服务端只保存 SHA-256 摘要。
- `tunnel_listen` 必须与主机 QUIC 的 `port` 不同（两者都是 UDP 监听），默认
  `0.0.0.0:14434`。托盘“允许防火墙”为主机端口与隧道端口各建一条按程序限定的
  UDP 入站规则；禁用隧道服务时会删除隧道规则。

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

环境边界：本机只有一张网卡、无法制造真实 NAT，因此上述直连走的是回环地址，
**没有验证跨网锥形 NAT 的打洞**；对称 NAT 下直连失败后切中继的行为由单元测试
覆盖，真机跨网需要两台位于不同网络的机器手动复核。中继统计中的字节数包含
两侧 QUIC 握手与重传，与编码器输出的裸帧大小不可直接比较。
