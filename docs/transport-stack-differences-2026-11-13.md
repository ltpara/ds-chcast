# 传输栈原理差异：QUIC + 自有穿透 vs WebRTC/ICE/STUN/TURN（2026-11-13）

本文回答一个比“功能表对齐”更上层的问题：RustDesk 用 WebRTC/ICE/STUN/TURN，
chast 用 QUIC + 自有打洞 + 自有中继端口租约，两者是否**在原理上**存在差异，
以及这些差异在拓扑覆盖、延迟结构、运维成本、故障域与演进路线上意味着什么。
功能面的逐项对照见 `docs/rustdesk-server-parity-2026-10-26.md`，NAT 场景矩阵
见 `docs/nat-traversal-2026-11-07.md`；本文不重复这两份文档的结论。

## 基线与方法

| 项目 | 口径 |
| --- | --- |
| 我方 | `deb64b8`（2026-11-12）＋工作区，协议 **v73**，Go 1.27.3 windows/amd64 |
| RustDesk 客户端 | `/d/rustdesk` @ `55786aa`（2026-11-03），`libs/hbb_common` 同检出 |
| 开源服务端 | `/d/ref/rustdesk-server` @ `680b603`（1.1.17，2026-11-08）；另于 2026-11-13 抽查 GitHub `master` 的 `src/rendezvous_server.rs`、`src/relay_server.rs` |
| 方法 | 两侧源码阅读为主；我方单机自动化与基准已执行；文中没有把“同名测试/同名函数”当作效果证据 |
| 证据等级 | 已实现 / 已构建 / 自动化通过 / 真实链路通过 / 待人工验证（与 §7 服务端文档一致） |

## 结论

原理差异成立，但不在“谁功能多”，而在**基础设施的归属**：RustDesk 把 NAT 穿透
交给标准化的 ICE/STUN/TURN（libwebrtc 实现），自己只做信令；chast 把会合、打洞、
中继全部自持，数据面用 QUIC。由此产生四类系统性差别：拓扑覆盖边界、延迟结构、
运维与成本画像、故障域与演进方式。四点都需要真实链路数据才能定量，本文给出
可执行的验收方法（第 4 节），而不是提前宣布优劣。

反过来，**不建议**把 WebRTC/ICE 引入 chast：它换来的能力（穿透 + 中继）我们已经
具备，而它带来的依赖（公共 STUN、第三方 TURN 凭据与可见性、SDP 解析面、DTLS/SCTP
分叉）与现有零拷贝、自持协议的设计目标冲突。需要补齐的是第 4 节里可度量的几项。

## 1. 先厘清对照物：RustDesk 的 WebRTC 不是浏览器那套

这一步决定对比是否失真。RustDesk 的 WebRTC 路径**没有 RTP/RTCP/SRTP、没有媒体
轨道、没有 jitter buffer**：它只取 ICE + DTLS + SCTP，建一条有序可靠的数据通道当
通用字节管道（`libs/hbb_common/src/webrtc.rs:708` 建 `bootstrap` 通道；非有序或
非全可靠通道被直接拒绝，`webrtc.rs:741-749`），视频、音频、控制原样跑在上面。
换句话说，WebRTC 在 RustDesk 里买到的是**一条能穿透 NAT 的通用连接**，媒体语义
仍由 RustDesk 自己定义。

它还在默认配置上刻意偏离标准 WebRTC：

- **拥塞控制默认关闭**：`webrtc::sctp::association::set_no_congestion_control(!cc)`
  （`webrtc.rs:653`），SCTP 不等拥塞窗口直接发，靠 fork 的 RTO 下限 1 s、F-RTO、
  1228 MTU、重排窗口压延迟；要 AIMD 需手动开 `allow-webrtc-congestion-control`。
- **只在公共会合服务器上默认启用**：`get_local_option` 对非 public 服务器且选项
  为空时返回 `"N"`（`src/common.rs:1341-1355`）；SOCKS 代理直接不建 offerer
  （`src/client.rs:612-620`），因为 ICE 自己开 UDP socket，会绕过代理暴露真实 IP。
- **策略中继但没配 TURN 就不建 WebRTC**（`src/client.rs:619`），直接回落 hbbr。

信令形态：SDP offer 放在 `PunchHoleRequest.webrtc_sdp_offer=12`，answer 放在
`PunchHoleSent.webrtc_sdp_answer=8` / `PunchHoleResponse.webrtc_sdp_answer=12`，
trickle 候选走 `IceCandidate`（`libs/hbb_common/protos/rendezvous.proto:46,83,110,
156,308`）。DTLS 指纹进入 `IdPk.dtls_fingerprint=3`，由服务器签名绑定 id/pk
（同文件 `:10`），防止会合/中继替换 SDP 里的指纹。

### 1.1 一个关键的事实：开源服务端目前不转发这些字段

`/d/ref/rustdesk-server` 1.1.17（`680b603`）与 2026-11-13 抽查的 master 中，
`src/rendezvous_server.rs`、`src/relay_server.rs` **没有任何 `webrtc` 或
`ice_candidate` 字面量**；`handle_punch_hole_request` 用逐字段重建的方式生成
`PunchHole`（`..Default::default()`，只拷 `socket_addr`/`nat_type`/
`relay_server`，`rendezvous_server.rs:861-866`），`PunchHoleRequest.webrtc_sdp_offer`
在服务器侧被丢弃，`IceCandidate` 也没有转发分支。

含义有两条，都必须写清楚：

1. RustDesk 的 WebRTC 能力依赖**转发这些字段的服务端**（其公共基础设施或更新的
   实现），不是开源 hbbs 即可获得的能力。自建开源服务器时，客户端会退到普通
   UDP/TCP 打洞或 hbbr 中继。这也解释了为什么客户端对非 public 服务器默认关闭
   WebRTC。
2. 因此本文对“RustDesk 一侧”的效果对比只能基于客户端代码语义与公开行为，不能
   假设开源服务端能复现其 WebRTC 路径。

## 2. 六层原理差异

| 层 | RustDesk | chast | 原理含义 |
| --- | --- | --- | --- |
| 发现/会合 | 服务器只做信令：观察源地址、下发对方地址与 relay 地址，SDP/候选在设计上按不透明字段转交（开源服务端当前版本连字段都不携带，见 1.1） | 服务器参与穿透：观测地址、回填候选、两个观测点判 NAT 类型、下发 `relay_hint` | 前者“转发消息”，后者“参与决策” |
| 穿透 | ICE 候选对探索 + 连通性校验，可发现 peer-reflexive 候选；默认 4 个公共 STUN（`webrtc.rs:164-169`） | 服务器观测地址配对 + 并行探测（`punch_request`/`punch_peer`），`punchTTL 30s`（`server/internal/rendezvous/rendezvous.go:33`），TCP punch 与 `/chast-signal` WebSocket 兜底 | ICE 是“探索并验证”，chast 是“配对并试探” |
| 中继 | 两条：TURN（标准、可第三方、可放 443）与 hbbr（自有，单监听按 uuid 配对任意多对端） | 只有自有中继：每台获租主机独占一个 UDP 端口（默认 `41000-41099`，`server/cmd/chast-server/main.go:72`）＋同号 TCP，端口内一个查看者槽位 | 中继的**成本与可见性归属**、并发模型不同 |
| 数据面 | 一条有序可靠 SCTP 通道承载一切（全局 HOL） | QUIC：1 条双向控制流 + 每个功能一条单向流（视频/麦克风/剪贴板/上传/端口转发/截图），音频走**不可靠 DATAGRAM**（`src/net/quic.cc:5291`；接收队列 256，满则丢最旧，`:65`、`:1419-1423`） | 音频尾延迟与流间隔离是结构性差异 |
| 加密与身份 | DTLS 自签证书，指纹经 SDP 传递并由 `IdPk.dtls_fingerprint` 纳入服务器签名；offer 要等 `secure_tcp_required` 成功后才发 | QUIC TLS 1.3 + 可固定服务器身份（`--server-pubkey`）+ 服务器 Ed25519 签名主机绑定 + 短时 transport 票 | **信任锚相同**（会合服务器），传输栈不同不影响信任模型 |
| 故障域 | 公共 STUN 挂了候选变少但仍可直连；TURN 挂了策略中继不可用；UDP 全封退 hbbr | 会合服务器是单点；UDP 封退 TCP punch/WS/TCP carrier；中继池耗尽降级为仅直连 | 前者发现面分散，后者集中可控、可审计 |

## 3. 系统级后果

### 3.1 拓扑覆盖的真实边界

ICE 能发现服务器没见过的路径（探测过程中 NAT 新开的映射，即 peer-reflexive 候选），
并做双向校验和快速失败判定；chast 的候选完全来自“服务器观察到的地址”，原理上拿
不到 peer-reflexive 候选。两端面对同一台服务器、观测点一致时多数场景等价；差异
出现在端口预测型 NAT、多级 NAT，以及“服务器观测地址不可用但第三方视角可用”的
网络里。更实际的一条：RustDesk 配了 TURN 就有 443/TLS 出口兜底，chast 要等价必须
自己把 carrier/中继放到 443。这是部署画像差异，不是代码能力差异——但必须写进
部署说明，否则单看代码会高估覆盖。

### 3.2 延迟结构

RustDesk 的 WebRTC 路径上音频、视频、控制共用一条**有序可靠**通道，视频丢包重传
会阻塞音频；它靠“去掉拥塞窗口 + 应用层丢帧”避免 cwnd 塌陷，代价是在共享链路上
可能自造排队和丢包。chast 把音频放数据报、控制放独立流，音频“不等 cwnd，满了就丢，
由播放端隐藏”（`src/net/quic.cc:5288-5291` 的注释即此语义），音频尾延迟与信令响应
有结构性优势；但视频仍跑在可靠流上，QUIC 拥塞控制遇丢包仍会退让。

两者其实是同一命题的两种解法：**“在可靠有序管道上跑实时媒体”**。RustDesk 用去掉
拥塞控制逼近不可靠；chast 用多流 + 数据报把不可靠隔离出来。后者不需要改协议方向，
需要的是 §4.E 的实测数据来证明隔离有效；当前的自动化只证明了音频传输本身（丢包、
隐藏、无解码错误），尾延迟仍需双机。

### 3.3 运维与成本

TURN 是第三方基础设施：带宽、配额、IP 可见性都在别人手里，配置里要放凭据，且
凭据不全的 TURN 条目会毒化所有连接（`webrtc.rs:417-437` 专门丢弃这类条目）。
chast 的中继是自己的：成本、限速、黑白名单、审计自控，代价是容量硬绑定在“每主机
一端口”（`server/internal/relay/relay.go:1209-1216` 对第二个查看者回 `busy`），
防火墙要放行 100 个 UDP 端口及其同号 TCP 监听。hbbr 单监听没有这个规划成本，但也
因此难以按会话做端口级隔离。这是既定取舍，不因功能对齐而消失。

### 3.4 故障域与可观测性

RustDesk 复用成熟的 ICE 校验算法与标准候选日志，但要维护 webrtc-rs 分叉（SCTP
定时器、MTU、IPv6 字节序与适配器结构补丁，见 `Cargo.toml:230-256` 的说明），且
代码里没有 ICE restart/renegotiate 路径，网络切换只能整段重连。chast 协议自持：
字段可读、审计有 correlation ID、诊断统一（`/metrics` 的
`chast_punch_requests_total`、`chast_relay_bytes_total`、
`chast_carrier_host_links_total`、`chast_ip_change_events_total`），代价是没有
第三方实现，所有 NAT 场景都要自己覆盖。

### 3.5 演进

QUIC 本身支持连接迁移与 NAT rebinding 路径校验，我们目前**没有配置也没有验证**
（`src/net/quic.cc:6061-6090` 只设了 LOW_LATENCY、发送不缓冲、0-RTT、datagram 与
流数量）。这是低成本可补的点，也是唯一可能领先 RustDesk 的地方；RustDesk 侧同样
没有迁移能力。

## 4. 验收方法

先给出总表，再逐项展开。所有“双机”项都需要两台不同网络的机器；本机没有真实
跨网环境，所以这些项在本文中一律标 **待人工验证**，不得按代码推断勾选。

| 编号 | 验收对象 | 当前自动化证据 | 真实链路 | 判据摘要 |
| --- | --- | --- | --- | --- |
| A | 直连成功率与路径分布 | 无（回环打洞测试不等于跨 NAT） | 待双机 | 20 轮 `path=direct` 比例，连同 `nat` 标注报告 |
| B | 对称 NAT 到中继的切换时延 | `direct_liveness_test.go`（回环兜底） | 待双机 | `relay_hint` 到 `path=relay` 的间隔 p95 |
| C | 会话中网络切换 | 无（无迁移，只有 8 s 空闲检测） | 待双机 | 恢复耗时、是否需重连、是否回直连 |
| D | UDP 全封与 443 等价 | `agent_tcp_test.go`（`--mode tcp` 回环）已通过 | 待双机 | 三档封堵下的路径与耗时 |
| E | 音频尾延迟与流隔离 | `quic_weak_network`（视频 ABR，不含音频）；`verify --netem --audio` 的音频断言与视频帧龄回归已通过 | 待双机 A/B | 音频丢包/隐藏次数；到达间隔 p99 |

> 2026-11-13 复跑修正：`quic_weak_network` 的断言只覆盖视频 ABR，音频默认关闭
> （`tools/quic.cc:10292-10295`，`local.audio = audio_probe`）。本节初稿把该用例当
> 作音频证据是错的；音频要加 `--audio`，实测结果见下。
| F | 中继容量与成本画像 | 基准与池耗尽测试已通过 | 待压测 | 每会话带宽 vs 端口数规划 |
| G | 信任锚等价性 | `role_keys_test.go`、`transport_tickets_test.go` 已通过 | 待跨网 | 伪服务器/替换中继被拒绝 |

### A. 直连成功率与路径分布

目的：给出“在我们关心的网络里，直连到底成不成”的可复现数字，而不是靠
“有打洞代码”推断。

前置（同一台服务器，双机可达）：

```sh
chast-server --http-listen 0.0.0.0:8443 --punch-listen 0.0.0.0:8444 \
  --relay-listen 0.0.0.0 --carrier-listen 0.0.0.0:8445 \
  --relay-ports 41000-41099 --server-key "$CHAST_SERVER_KEY"
```

主机侧与查看侧代理（查看侧 stdout 是 JSONL 事件流，诊断在 stderr）：

```sh
chast-tunnel host --server http://SERVER:8443 --server-key-stdin \
  --id h1 --local 127.0.0.1:4433 --listen 0.0.0.0:14434 --verbose

chast-tunnel viewer --server http://SERVER:8443 --link-key-stdin \
  --id h1 --mode auto --direct-history-dir "" --verbose 2>viewer.log | grep '"event":"path"'
```

`--direct-history-dir ""` 关闭每对端的直连历史，否则后几轮会复用前几轮的超时预算，
20 轮不再是独立样本。

步骤：查看端连续 20 轮启动/关闭，每轮间隔 30 s；记录每轮 `path`、`transport`、
`nat`、`candidates` 与首条 `ready` 的耗时。服务器侧交叉核对：

```sh
curl -s -H "Authorization: Bearer $CHAST_SERVER_KEY" http://SERVER:8443/metrics | grep -E \
 'chast_(punch_requests_total|relay_bytes_total|carrier_host_links_total|relay_lease_missing_total)'
```

判据：报告“direct x/20、relay y/20、carrier z/20”以及各自耗时中位数；同一网络
用 RustDesk 客户端连接同一对端，按客户端日志里的连接类型（UDP/TCP/WebRTC/Relay；
开源服务端上不会出现 WebRTC，见 1.1）记同样口径。**不设硬门槛**——结论必须带
NAT 类型标签，因为“打不通”可能来自网络而实现问题另有其证（见 B、D）。

### B. 对称 NAT 到中继的切换时延

目的：对照 RustDesk 的 2.5 s 中继兜底（`src/client.rs:636`
`RELAY_FALLBACK_DELAY_MS`），确认我们从中继决策到路径就绪的时延是否可接受。

触发方式（二选一）：

1. 真实对称 NAT：查看端走运营商热点（常见 CGNAT），主机在家庭宽带。
2. 机械上限（不需要 NAT）：`--direct-timeout 3s` 强制 auto 在 3 s 后放弃直连，
   测兜底机制本身的下界；它验证链路，不验证 NAT 判定。

步骤：在主机日志里找 `symmetric NAT; skipping the v4 punch`
（`server/internal/agent/host.go:1331`）或查看端 `host chose the relay; ending the
direct attempt`（`server/internal/agent/viewer.go:1681-1694`），记录到查看端出现
`"event":"path"` 且 `path=relay` 的时间差；重复 10 次。再跑一次 `--mode relay`
记录纯中继建立耗时作为下界。

判据：`relay_hint → 中继可用` 的 p95。若超过 2.5 s，说明“已判定打不通但用户仍要
等”，应缩短 punch 节奏或让 `relay_hint` 更早发出；若显著低于 2.5 s，则该差异不
构成体验差距。验收时要同时记录 NAT 判定来源（真实对称 / `--direct-timeout`）。

### C. 会话中网络切换

目的：确认现状与 RustDesk 的等价程度。**当前两边都没有连接迁移**：chast 在直连
8 s 无回包后判定丢失并切到中继/carrier，且不回切直连
（`server/internal/agent/direct_liveness.go:11-13,40-48`；`fallBackToRelay` 里
`latched` 语义见 `viewer.go:1644-1668`）。

步骤（Windows，需管理员）：

```powershell
Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false
Start-Sleep -Seconds 5
Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
```

会话中执行，记录三件事：

1. 服务器 `chast_ip_change_events_total` 是否 +1（认证保活观察到公共地址变化）；
2. 查看端日志 `UDP direct path was lost` 到 `"event":"path"` 的时间；
3. 上层 QUIC 会话是否存活（画面恢复 vs 必须重连），以及再切回后是否回直连
   （预期：否，`latched`）。

判据：给出“恢复可用耗时 / 是否需要重连”两个数字，与 RustDesk 同场景对照
（RustDesk 无 ICE restart，预期同样是整段重连）。若我们显著更慢，优先看 8 s 检出
是否能缩短；若要“切回可回直连”，单独立项做 QUIC 路径校验/迁移，验收标准另定为
“切换后 <3 s 恢复、30 s 内回到直连”。

### D. UDP 全封与 443 等价

目的：验证 UDP 封锁下的兜底阶梯的真实可用性，并给出“不开放 UDP 的客户网络”下的
部署等价方案。

单机自动化（已通过，作为回归基线）：

```sh
cd server && go test ./internal/agent -run 'TCPPunch|Carrier' -count=1
```

真实链路（Windows 防火墙，出站阻断）：

```powershell
# 第 1 档：只封会合 UDP 打洞端口，预期走 punched TCP
New-NetFirewallRule -DisplayName chast-block-punch -Direction Outbound `
  -Protocol UDP -RemotePort 8444 -Action Block

# 第 2 档：再封中继 UDP，预期走 TCP carrier（--carrier-listen 8445）
New-NetFirewallRule -DisplayName chast-block-relay -Direction Outbound `
  -Protocol UDP -RemotePort 41000-41099 -Action Block

# 第 3 档：同时封 carrier 的 TCP，预期无路可走（用于确认失败可见、不是静默挂起）
New-NetFirewallRule -DisplayName chast-block-carrier -Direction Outbound `
  -Protocol TCP -RemotePort 8445 -Action Block

# 清理
Remove-NetFirewallRule -DisplayName chast-block-punch,chast-block-relay,chast-block-carrier
```

每档记录查看端 `"event":"path"` 的 `path`/`transport` 与首条 `ready` 耗时，服务器
侧看 `chast_carrier_host_links_total`、`chast_punch_requests_total`。

判据：

- 第 1 档：UDP 打洞被封后应落到 punched TCP（`path=direct`、`transport=tcp`），
  建立耗时应落在 3/6/9 s 打洞节奏加 TCP 协调预算之内。
- 第 2 档：代理的 UDP 中继不可用后应落到共享 carrier（`path=carrier`）；若仍在
  `path=connecting` 停留超过 15 s，视为缺陷。
- 第 3 档：代理已无可用传输，必须给出明确的 `"event":"error"`，不得静默挂起。

一个必须记录的边界：中继端口本身另有 TCP/WS 载体入口
（`server/internal/relay/relay.go:1410-1479`），供不能发 UDP 的查看端直接连接；
`chast-tunnel viewer` 代理不会自动改用它，所以“UDP 全封仍可中继”目前只对 carrier
成立。RustDesk 的 hbbr 中继本身走 TCP，UDP 被封时天然可用——这是它结构上的优势，
若要补齐，方向是让代理在中继 UDP 失败后复用中继端口的 TCP 入口，而不是引入新协议。

443 等价（部署配方，本机未验证）：把 carrier（或独立中继节点）放到 443，例如
`chast-relay --carrier-listen 0.0.0.0:443`，由前置 TLS/WS 终结；或在独立中继节点用
`--relay-ports 443`（两者不要在同一实例上同时占用 443）。文档必须注明：这是
“等价于 TURN 443 出口”的唯一方式，不改协议。

### E. 音频尾延迟与流隔离

目的：证明“音频走 QUIC DATAGRAM、不进可靠流”在真实坏链路上确实把音频从视频重传
里隔离出来。

先明确 ctest 实际覆盖什么。`quic_weak_network`（`CMakeLists.txt:947-949`）执行
`chast-quic verify --port 45513 --netem --quiet`，链路 profile 是 **80 ms 单程 +
20 ms 抖动、3% 丢包、1 Mbps 瓶颈、t=4 s 起劣化、t=12 s 恢复、至少 25 s**，断言
全部在视频侧（ABR 降档/回升、每帧到达、无缝号缺口、帧龄 <3 s、几何不变）。该用例
默认 **不含音频**：`tools/quic.cc:10292-10295` 只在传入 `--audio` 时才打开音频探针。

音频要单独跑，`--audio` 会打开真实断言（`tools/quic.cc:10492-10525`）：

```sh
# 音频走 DATAGRAM（默认）
build/root/Release/chast-quic.exe verify --port 45523 --netem --audio
# A/B：同一坏链路，音频改走可靠流
build/root/Release/chast-quic.exe verify --port 45523 --netem --audio --audio-stream
```

2026-11-13 本机实测（合成 440 Hz 音调、Opus 48 kHz、无播放设备参与）：

| 传输 | 发送/接收 | 丢包 | 序号缺口 | 重排 | 隐藏 | 解码时长 | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| DATAGRAM | 2501 / 2464 | 1.48% | 70 | 112 | 131（≤ 缺失 149） | 24.83 s | 全部音频断言通过 |
| 可靠流（A/B） | 2499 / 2499 | 0% | 0 | 0 | 0（0.00% loss、concealed 0） | 24.99 s | 全部断言通过 |

两者都满足“无 CRC/解码/协议错误”“隐藏帧数不超过缺失帧数”，DATAGRAM 侧还满足
“丢包 ≤10%”。这说明坏链路上两种传输都能把音频送全或可控丢弃，**协议层不存在
“视频重传阻塞音频”的路径**：发送端不等 cwnd、满了即丢
（`src/net/quic.cc:5288-5300`），接收端队列满丢最旧（`:1419-1423`）。

方向与既有记录一致：`docs/roadmap.md:214`（2026-10-31）在同一 profile 上记录
DATAGRAM 1248/1251（0.24% 丢）、可靠流 1250/1250，并注明“欠载/丢块与加 PLC 前
同一量级，音乐级连续需要自适应抖动缓冲”。本次数值差异来自运行时长（25 s）与本机
负载，不能据此推断退化或改进。

**没有测到的**：音频到达间隔 p99 与欠播次数。`--play` 会接真实 WASAPI 渲染器并
报告 `underruns`/`starved ms`，但在本机它不能当效果证据——作为对照，**干净回环**
（不加 `--netem`）同样出现 `starved 870 ms / dropped 94`，而 `verify` 把主机、
查看端与中继放在同一进程，1440p 视频编解码与音频推流争用 CPU。同等条件下开
`--audio` 曾把视频帧龄从 1740–2418 ms（纯视频 5 次）抬高到 2415–3301 ms，
并复现过一次 4175 ms 超过 3 s 预算的失败。2026-11-14 已定位并修复该回归：
未完成的应用层视频回执超过 2 s 后，旧逻辑要等第二次停顿采样才请求降码率，且只使用
普通未确认档的 0.8 步长；现在首个超过 2 s 的确认停顿即按 0.3 强降并受既定质量/
1 Mbps 下限约束，同时仍保留 1 s 防抖，2 s 内的探测抖动不变。`video_qos` 定向单元
测试覆盖“2 s 内不切、首个超 2 s 回执即切、清空停顿后 FPS 与码率仍能恢复”。复跑
三种本机弱网形态均通过：纯视频最大帧龄 2873 ms，DATAGRAM 音频 1956 ms，可靠流音频
1886 ms。因此本项结论是：

- 音频**传输**在坏链路上已有自动化证据（上表）。
- 音频**尾延迟/欠播**属于 **待人工验证**：需双机 + 真实查看器播放，或把 `verify`
  的播放探针与视频负载分进程后再测。
- 同进程负载下的帧龄超预算已修复并有自动化回归；纯视频 2873 ms 的本机余量仍偏薄，
  但不影响本次“回执停顿驱动的慢降档”根因结论。真实双机跨网仍需复验。

### F. 中继容量与成本画像

目的：把“每主机一端口”的容量边界变成可计算的运维参数，避免再次出现池耗尽才被
发现（该失败模式已于 2026-11-12 改为降级注册，见服务端文档 §7.1）。

已知事实：一个端口一个查看者槽位/地址族（`relay.go:1209-1216`），默认 100 端口
即 100 个并发中继会话/实例（v4；v6 槽位独立另算）；hbbr 单监听按 uuid 配对。

判据：

```sh
cd server
go test ./internal/relay -run '^$' -bench 'Benchmark(RelayLease|RelayForward|RelayBurst)$' \
  -benchmem -count=5 -benchtime=2s
go test ./internal/api -run 'ExhaustedRelayPool' -count=1
```

`BenchmarkRelayBurst` 的中位数（当前 79 MB/s，单核回环、关闭整形）约为默认单会话
上限 128 Mbit/s 的 4.9 倍，低于 4 倍即说明转发实现而非策略成为瓶颈；同时按
“并发中继会话 ≤ 端口数 × 地址族”规划端口范围，并在压测中确认池耗尽只降级、
不导致主机整体下线。

### G. 信任锚等价性

目的：确认“传输栈不同”不影响身份绑定强度。RustDesk 把 DTLS 指纹纳入服务器签名
（`rendezvous.proto:10`）；chast 用服务器 Ed25519 主机绑定 + QUIC TLS + 短时
transport 票。

判据（自动化已覆盖服务端侧）：

```sh
cd server
go test ./internal/api -run 'RoleKeys|TransportTickets' -count=1
go test ./internal/agent -run 'TCPPunchRejectsRelayOnly' -count=1
```

真实链路项：在双机环境用一台伪服务器返回伪造的 `relay`/`punch` 地址，确认客户端
拒绝或无法建立会话，且审计/日志有记录。当前状态：服务端与代理的拒绝路径为
**自动化通过**，跨网中间人验证为 **待人工验证**。

## 5. 结论与边界

1. **原理差异是“自有窄栈 vs 标准宽栈”**，不是能力代差。需要对齐的是可度量项
   （A–G），不是传输栈本身。
2. **唯一建议补的能力**是 QUIC 连接迁移/NAT rebinding 的验证与启用（C 的后半段）。
   RustDesk 侧没有该能力，这属于低成本、可领先的点；但它必须在双机上验证，不能
   仅凭 QUIC 的协议能力声明。
3. **明确不做**：引入 ICE/STUN/TURN、解析或生成 SDP、把公共 STUN 作为默认依赖。
   理由见 §3.3：会把穿透与中继的成本、可见性和凭据管理转移到第三方，并破坏
   “零拷贝 + 自持协议”的既有约束。
4. **文档必须随部署走**：A/D 的结论与 443 配方要写进 `server/README.md` 的部署
   段，否则“原理上覆盖”会被误读为“默认配置下覆盖”。

## 6. 复现与证据索引

上述所有自动化项：

```sh
cd server
go test ./internal/agent -run 'TCPPunch|Carrier|UDPDirect|TCPBlackhole' -count=1
go test ./internal/rendezvous -run 'Punch|LAN|NAT' -count=1
go test ./internal/api -run 'RoleKeys|TransportTickets|ExhaustedRelayPool' -count=1
go test ./internal/relay -count=1
```

单机 CTest（弱网与音频，构建目录内；`-C Release` 对应本机 VS 多配置生成器）：

```sh
ctest --test-dir build/root -C Release -R '^quic_weak_network$' --output-on-failure
ctest --test-dir build/root -C Release -R 'audio' --output-on-failure
```

弱网用例的音频变体（不走 ctest，直接驱动二进制）：

```sh
build/root/Release/chast-quic.exe verify --port 45523 --netem --audio
build/root/Release/chast-quic.exe verify --port 45523 --netem --audio --audio-stream
```

“开源服务端不携带 WebRTC 字段”这一事实的复核方式（2026-11-13）：

```sh
grep -rn -i "webrtc\|ice_candidate" /d/ref/rustdesk-server/src/   # 无输出
grep -n -A6 "set_punch_hole(PunchHole" /d/ref/rustdesk-server/src/rendezvous_server.rs
```

第一条无输出即表示该版本不实现这些字段；第二条可见 `PunchHole` 逐字段重建、
`..Default::default()` 丢弃其余字段。抽查 GitHub master 用同两条命令的结果相同。

## 7. 状态汇总

- **已实现**：§2 表中全部机制；§3.4 列出的指标；C 项的 8 s 检出与中继锁定；
  D 项的 TCP punch / WS / carrier 三档；F 项的池耗尽降级。
- **已构建**：本文引用的版本均已编译通过（Go 侧 `go vet` 与测试；C++ 侧既有
  构建产物）。
- **自动化通过**：A–G 表中“当前自动化证据”列出的测试与基准。2026-11-13 复跑：
  `internal/api`（RoleKeys/TransportTickets/ExhaustedRelayPool）、`internal/agent`
  （TCPPunch/Carrier/UDPDirect/TCPBlackhole，46.6 s）、`internal/rendezvous`
  （Punch/LAN/NAT）、`internal/relay` 全包均通过；`quic_weak_network` 5/5 通过
  （单次 29.9–30.0 s，`--repeat until-fail:4` 全过）；`audio`/`audio_dsp`/
  `audio_playout` 3/3 通过；弱网音频探针 DATAGRAM 与可靠流 A/B 均通过（§4.E）。
  2026-11-14 复跑 `video_qos`、`quic_weak_network`、`audio`/`audio_dsp`/
  `audio_playout` 全过，并重跑三种弱网形态，最大帧龄分别为 2873/1956/1886 ms。
- **真实链路通过**：本文无新增。此前的单机真实进程回环（双栈注册/中继、carrier、
  软件更新）不构成跨网证据。
- **待人工验证**：A–G 的全部双机/跨网项；C 的 QUIC 迁移可行性；D 的 443 部署
  配方；E 的音频到达间隔 p99 与欠播（`--play` 在本机单进程下不可作证据，见 §4.E）；
  G 的跨网中间人实验。
- **2026-11-14 弱网帧龄回归已修复**：复现路径为
  `verify --port 45523 --netem --audio`，修复前最大帧龄 4175 ms；首个超过 2 s 的
  未完成应用回执现在立即触发受下限约束的 0.3 强降，而不是等待第二次停顿或使用 0.8
  普通步长。修复后同命令最大帧龄 1956 ms；纯视频与可靠流音频分别为 2873 ms 和
  1886 ms。以上均为本机单进程弱网证据，不替代真实双机/跨网验收。
