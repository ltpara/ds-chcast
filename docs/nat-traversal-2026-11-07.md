# 打洞与 RustDesk 对齐（2026-11-07）

状态：代码与单机回环验证完成；跨网双机验收待做。
范围：NAT 类型测试、打洞请求/探测重试、IPv6 并行打洞、同网段直达、TCP 打洞。
明确不做：WebRTC/ICE/STUN/TURN、直接 IP 访问入口。
相关代码：`server/internal/agent/viewer.go`、`host.go`、`tcppunch.go`、
`tcpsock*.go`、`server/internal/rendezvous/tcppunch.go`。

## 1. 目标与判定口径

RustDesk 的打洞经过大量真实网络验证，本轮按“功能一比一”对齐，而不是
另造一套策略。判定标准：

- 每项行为的**触发条件、重试节奏、超时与回退顺序**与 RustDesk 一致；
- 失败时显式回退（中继/载体），不把未确认的路径报告成直连；
- 单机回环只能证明逻辑与数据面，不能证明 NAT 穿越成功率；成功率由
  第 6 节的双机清单验收。

## 2. 与 RustDesk 的对照

| 能力 | RustDesk | chast 实现 |
| --- | --- | --- |
| NAT 类型 | 主机比较会合/中继两个观察端口；连接方后台 `test_nat` | 主机保留原逻辑；查看者新增 `nat_probe`/`nat_probe_ack`，会合与中继两个观察点，3 次尝试、每次 750 ms |
| 对称 NAT | 缩短直连等待（`MIN` 超时），v6 仍并行 | 查看者 v4 直连预算缩到 1 s；主机跳过公网 v4 探测，但继续探测 UPnP、IPv6、同网段 |
| 打洞请求 | `PunchHoleRequest` 三次，等待窗口 3 s/6 s/9 s | `punch_request` 同样的节奏（首次随查询发出，3 s 与 9 s 各重发一次） |
| 探测与确认 | `punch_udp`：随机事务号，20 ms 起 1.5× 退避到 200 ms，`MAX_TIME` 3 s | `punch_probe`/`punch_ack`，同样节奏；只有本轮事务号的 ACK 或真实主机数据才锁定 |
| IPv6 | `punch_v6` 与 v4 并行；`local_addr_v6` 自报 | `self_v6` + `candidate_v6`/`local_addrs_v6`；双栈代理并行探测；没有全局 v6 源时不发布 v6 候选 |
| 同网段 | `FetchLocalAddr` + intranet 路径，不受对称 NAT 结论影响 | 仅当双方观察公网一致时转发局域网候选；主机并行探测，不看对称 NAT |
| UPnP | 映射外部端口随打洞交换 | 保留：`upnp_port` 随 keepalive/punch 交换，对称 NAT 下仍探测该端点 |
| TCP 打洞 | 双方经 hbbs 的 TCP 通道交换地址；主机预打洞后带 `SO_REUSEADDR` 监听并反拨；18 s 窗口 | 独立协调监听（默认与 UDP 会合同端口号）；同样的预打洞 + 监听/反拨；18 s 窗口；数据用载体帧承载 |
| 回退顺序 | 直连失败 → 中继 | `auto`：UDP/TCP 直连 → 中继；`carrier` 显式强制共享载体 |

与 RustDesk 的实现差异（不影响功能等价）：

- RustDesk 在 hbbs 的同一条复用 TCP 连接上协调 TCP 打洞；chast 用独立
  TCP 监听和长度前缀帧，配对语义相同，且不依赖复用连接的生命周期。
- chast 的链路承载的是不透明 QUIC 数据报，TCP 打洞只替换传输，不改变
  QUIC 的端到端认证与加密。
- RustDesk 的连接方 NAT 测试用于选择直连/中继；chast 把结果随
  `punch_request` 交给主机，由主机决定是否跳过 v4。

## 3. 数据流

### 3.1 UDP 打洞

```
viewer                         hbbs                          host
  │ GET /v1/hosts/{id}           │                              │
  │── punch_request ────────────>│                              │
  │   (nat_type, self_v6,        │── punch_peer ───────────────>│
  │    local_addrs*)             │<─ keepalive(punch) ──────────│
  │<───────────── punch_peer ────│                              │
  │                              │                              │
  │── punch_probe(tid) ─────────────────────────────────────────>│
  │<───────────────────────────────────────────── punch_ack(tid)│
  │<════════════════ 真实 QUIC 数据报 ═════════════════════════>│
```

`local_addrs` 只有在双方的观察公网地址一致时才转发给主机。

### 3.2 TCP 打洞

```
viewer                        hbbs TCP 端口                    host
  │ TCP 连接 + tcp_punch(viewer, nonce, HMAC) ──>│               │
  │                                              │── UDP tcp_punch(nonce) ×3 ──>│
  │                                              │<── tcp_punch(host, nonce, 签名)
  │<──────────── tcp_punch(candidate=host 观察地址)              │
  │                                              │────── tcp_punch(candidate=viewer 观察地址) ──>│
  │                                                              │
  │  双方用各自协调连接的本地端口向对方 candidate 拨号            │
  │  主机从该端口预打洞 30 ms，随后在该端口监听并反拨             │
  │<════════════ tcp_direct（主机签名）→ 查看者校验 ════════════>│
  │<════════════ 载体帧承载的 QUIC 数据报 ═════════════════════>│
```

关键点：

- 协调服务器只配对地址，不转发会话数据。
- 主机在协调连接关闭后，用它**自己的本地端口**重新监听；这个端口就是
  先前向查看者预打洞的源端口，因此对称 NAT/防火墙已经放行回程 SYN。
- 链路首条报文是签名 `tcp_direct`（`chast-tcp-direct-v1`，覆盖 host id、
  nonce、时间戳）；查看者用查询得到的 host 公钥验证后才发送 viewer hello。
- 链路空闲 60 s 断开；20 s 空帧保活；写入 5 s、握手 10 s 超时。

### 3.3 套接字分工

查看者有两个 UDP 套接字：`127.0.0.1` 本地套接字只服务本地 QUIC 客户端，
可路由上游套接字负责会合、探测、打洞与转发。这样上游收到的陌生数据报
不会污染本地 QUIC 客户端，也允许查看者把本地监听与对外绑定分开。
主机使用同一个数据套接字完成保活、打洞、中继与本地转发；仅当本地控制
地址属于另一地址族时另开一个只用于本地回环的套接字。

### 3.4 超时与模式

| 场景 | 直连预算 | 行为 |
| --- | --- | --- |
| `auto`，双方非对称 NAT | `--direct-timeout`（默认 3 s） | UDP 与 TCP 并行；任一条确认即锁定；超时切中继 |
| `auto`，任一方对称 NAT | 1 s | 跳过公网 v4；UDP 只探 UPnP/IPv6/同网段，TCP 并行；超时切中继 |
| `direct` | UDP 三次请求（等待窗口 3/6/9 s）+ 一次 3 s 探测；TCP 18 s | 两条路径都试；TCP 在窗口内反复重拨；没有中继回退 |
| `tcp` | 18 s | 只走 TCP 打洞；UDP 打洞与中继不启动（本地/上游 UDP 套接字仍承载数据报） |
| `relay`/`carrier` | — | 强制中继/共享载体 |

## 4. 安全边界

- 查看者 TCP 报到：`chast-punch-v1` HMAC（server-key），±60 s，nonce 8–64；
- 主机 TCP 报到：`chast-tcp-punch-v1` Ed25519 签名，只有注册私钥能代表该 ID；
- 配对键为 `host_id + nonce`；重复报到替换旧等待，会话 30 s 过期；
  单 IP 每 10 s 最多 60 次报到，全局最多 1024 个进行中的会话；
- 主机对同一 nonce 去重，最多 8 个并发的 TCP 打洞；
- 打洞链路的首条报文由主机签名；查看者不签名，因为链路上层仍是端到端
  认证的 QUIC，链路只负责搬运数据报——这与 RustDesk 的定位一致；
- `relay-only` 服务器拒绝 TCP 打洞报到（监听仍绑定，报到一律返回错误），
  并在查询中隐藏 `tcp_punch`，查看者不会拿到一个必然被拒绝的地址。

## 5. 单机回环验证（2026-11-07）

`go test ./...` 全部通过。与本轮直接相关的用例：

| 用例 | 断言 |
| --- | --- |
| `TestViewerDetectsOwnNATType` | 查看者用两个观察点区分 `asymmetric`/`symmetric` |
| `TestViewerSymmetricNATShortensDirectWait` | 对称 NAT 把直连窗口压到 1 s |
| `TestHostDetectsNATType` | 主机保持会合/中继双点分类 |
| `TestHostSkipsV4PunchForSymmetricNAT` | 对称时跳过公网 v4 |
| `TestHostPunchesViewerLocalCandidatesUnderSymmetricNAT` | 对称时仍探测同网段候选 |
| `TestHostStillPunchesIPv6ForSymmetricNAT` | 对称时仍探测 IPv6 |
| `TestHostPunchesBothFamiliesInParallel` | v4/v6 并行 |
| `TestViewerPunchProbeAndAck` | 事务号探测/确认；单向来包不锁定 |
| `TestViewerPunchRequestReportsSelfV6` / `ReportsLocalAddrs` | 候选上报内容 |
| `TestViewerDropsIPv6CandidatesWithoutGlobalV6` | 无全局 v6 源时不发布 v6 |
| `TestHostAnswersPunchProbeOnlyForAnnouncedViewer` | 只应答被通知的查看者来源 |
| `TestNatProbeReportsObservedSource` / `TestNatProbeDropsShortNonce` | NAT 观察点与长度校验 |
| `TestTCPPunchPairsBothHalves` | 两侧拿到对方套接字的观察地址 |
| `TestTCPPunchRejectsBadHostSignature` / `RejectsViewerWithoutAuth` | TCP 协调鉴权负向 |
| `TestTCPPunchPathThroughAgents` | 真实 TCP 打洞链路完成 QUIC 数据报回环，`path=direct`、`transport=tcp` |
| `TestTCPPunchRejectsRelayOnly` | relay-only 下 `--mode tcp` 启动失败 |

链路生命周期对应的日志事件：`TCP punch paired`（debug）、
`TCP direct path established`、`TCP viewer path established`、
`TCP viewer path closed`；双机验收时用 `--verbose` 采集。

## 6. 双机验收清单

### 6.1 环境

- 两台位于不同网络的机器（以下称 A=查看者、B=主机），一台有公网地址的
  服务器（或至少 A/B 都能访问）；
- 服务器放行：HTTP 端口、UDP 会合端口、**同一端口号的 TCP**（TCP 打洞
  协调）、UDP 中继端口范围、TCP 载体端口；
- A/B 安装同版本 `chast-tunnel`/`chast-host`/`chast-quic`；
- 不使用会在客户端改写 UDP 的企业代理；如使用 SOCKS5/HTTP 代理，代理只能
  承载共享载体：`auto`/`relay` 会被强制到 `carrier`，`direct`/`tcp` 直接
  拒绝启动。

服务器：

```sh
chast-server --http-listen 0.0.0.0:8443 \
  --punch-listen 0.0.0.0:8444 \
  --relay-listen 0.0.0.0 --relay-ports 41000-41009 \
  --carrier-listen 0.0.0.0:8445 \
  --advertise <public-ip> --server-key <key> \
  --state chast-server-state.db --identity chast-server-identity.key
```

启动日志应包含 `TCP punch listening <addr>:8444` 与
`TCP punch advertised <public-ip>:8444`。`--tcp-punch-listen off` 可关闭
TCP 打洞；显式地址用于服务器 NAT/端口映射与 UDP 不同的时候。

主机 B：

```sh
chast-tunnel host --server https://<server> --server-key-stdin \
  --id hostB --key <host-secret> --local 127.0.0.1:4433 \
  --carrier 127.0.0.1:4434 --listen 0.0.0.0:14434 --upnp
```

查看者 A：

```sh
chast-tunnel viewer --server https://<server> --server-key-stdin \
  --id hostB --listen 127.0.0.1:24434 --mode auto --verbose
```

两条命令都从 stdin 第一行读服务器密钥，例如在 bash 中用
`<<< "$CHAST_SERVER_KEY"` 重定向；密钥不进入 argv 或日志。

### 6.2 用例与预期

| # | 网络条件 | 查看者命令 | 预期路径 | 判定 |
| --- | --- | --- | --- | --- |
| 1 | A/B 均为锥形 NAT | `--mode auto` | UDP 直连 | `path=direct`；会话期间中继字节为 0 |
| 2 | A/B 同一局域网，路由器不支持 hairpin | `--mode auto` | 同网段直连 | 主机日志含 `punching the viewer on the local network`；`candidate` 为内网地址 |
| 3 | A 在对称 NAT（如手机热点） | `--mode auto` | IPv6/TCP 直连，或 1 s 后中继 | 主机日志含 `symmetric NAT; skipping the v4 punch`；切换时间 ≈1 s |
| 4 | 服务器/主机放行 TCP 但丢弃全部 UDP | `--mode tcp` | TCP 直连 | `transport=tcp`；建立时间 ≤18 s；随后 `auto` 在 `--direct-timeout`（默认 3 s）内也应走 TCP |
| 5 | UDP 与 TCP 均不可直连 | `--mode auto` | 中继 | `path=relay`；中继字节 >0；切换时间符合预算 |
| 6 | 双方有全局 IPv6 且防火墙放行 | `--mode direct` | IPv6 直连 | 查看者/主机候选含 v6；日志 `punching the viewer over IPv6` |
| 7 | 服务器加 `--relay-only` | `--mode tcp` / `auto` | `tcp` 启动报错；`auto` 走载体/中继 | 查询无 `tcp_punch`，无直连候选 |
| 8 | 同主机两个查看者 | 两个 `--mode tcp` 并发 | 两条 TCP 链路并存 | 关闭其中一个，另一个帧流不中断 |
| 9 | TCP 链路中途断开（禁用网卡/拔线） | `--mode auto` | 回退中继 | 日志 `TCP direct path was lost; using relay`；会话不中断 |
| 10 | 目录/令牌/防火墙变更 | `--mode tcp` 并删除服务器 TCP 放行 | 失败并回退 | `auto` 在预算内回退中继；`tcp` 18 s 后日志 `TCP punch was not confirmed`，代理继续运行 |

### 6.3 每个用例要采集

- 查看者 stdout：`ready` 与 `path` 事件（`path`、`transport`、`remote`、
  `direct`、`relay`）；
- 主机 stdout/stderr：`path established`、`TCP viewer path`、`symmetric NAT`
  与打洞相关日志；
- 服务器 `GET /v1/hosts/hostB`：`candidate`、`candidates`、`candidate_v6`、
  `nat_type`、`tcp_punch`、`transport`；
- `/v1/stats`：对应中继端口的 `to_host_bytes`/`to_viewer_bytes`（直连应为
  0 或仅控制流量）；
- 会话摘要：`received/decoded`、`crc-errors`、`decode-errors`、`gaps`、
  `digest`。

### 6.4 通过标准

- 直连用例：会话数据不经过服务器中继（中继字节为 0），帧完整、0 CRC/
  解码错误；
- 回退用例：在预算内完成切换，切换后会话继续，QUIC 不重连；
- 重复性：每个场景连续 3 次，路径稳定；允许首次因 NAT 映射空闲而稍慢；
- 安全负向：错误 server-key、错误主机签名、过期 nonce 的 TCP 报到被拒绝。

## 7. 已知限制

- 本地只有一台开发机、单网卡、无真实 NAT；对称 NAT 分类、跨网锥形 NAT、
  IPv6 与 TCP 打洞的真实成功率只能由第 6 节验收给出。
- 运营商级 NAT（CGNAT）与严格企业防火墙可能同时封 UDP 与入站 TCP；
  此时只有中继/载体路径可用，`auto` 会按预算回退。
- UPnP 只处理 IPv4 WANIPConnection/WANPPPConnection；IPv6 防火墙规则不在
  控制范围。
- TCP 打洞协调端点使用服务器对外通告的 `--advertise` 主机名/IP；若服务器
  位于端口映射后，UDP 与 TCP 必须映射到同一端口号，或用
  `--tcp-punch-listen` 单独指定可达地址（该值同时用于监听与通告）。
- 真实双机握手、突发丢包与弱网下的 TCP 链路表现（队头阻塞、重传）仍需
  结合 `docs/weak-network-acceptance.md` 的弱网工具复核。
