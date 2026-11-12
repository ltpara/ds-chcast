# 打洞与 RustDesk 对齐（2026-11-08 修订）

范围：TCP 触发、预打洞顺序、连接预算、回退竞态，以及打洞后的保活和会话重连；保留现有 QUIC 数据报传输。
参照本地 `D:/rustdesk-ref`（`7423dced37845c48093b6f71bd82e4a16cc8c79d`）和
`D:/ref/rustdesk-server`（`873230e35a8805de7fd057e7eb763c46f7a174ef`）。
本机测试验证流程、鉴权及数据往返；第 6 节真实跨网成功率尚未验收。

## 1. 修复后的对照

| 项目 | RustDesk 参考位置 | 当前行为 |
| --- | --- | --- |
| 鉴权后通知 | 服务端 `rendezvous_server.rs:722`；客户端 `rendezvous_mediator.rs:418` | 主机主动建立独立 TCP 会合控制连接；服务端鉴权后通知；UDP 备用，无需查看者 carrier 令牌、会话或配额 |
| 预打洞顺序 | `rendezvous_mediator.rs:701`；`server.rs:176` | 主机复用协调端口预拨，最长 30 ms；随后签名报到，服务器才释放查看者；关闭协调连接后复用端口监听 18 s，不并行反拨 |
| 同网段 | `FetchLocalAddr` / intranet | 观察 IP 相同，或会合服务配置 `--lan-mask` 后双方观察地址都在网段内，才返回主机签名的内网 TCP 端点，免公网预拨，连接预算 1 s；UDP 保留 LAN 候选 |
| 请求重试 | `client.rs:449` | 同一 TCP 连接、同一 nonce，分别等待 3/6/9 s；重试只通知主机，不新建等待会话；断开清理；读取响应不因重试丢失半帧 |
| 连接预算 | `client.rs:649` | 按下表实现分支顺序，协调时间不提前耗尽实际连接预算 |
| 历史结果 | `client.rs:210`、`:2063` | CLI 按服务器/主机 ID/公钥保存上次直连失败，成功清零，回退置一 |
| 路径竞速 | `client.rs:684` | TCP、UDP/IPv6 并行；选定后取消其他尝试；默认全部结束才回退；主机事先判定公网 v4 打洞不可行时提前回传中继决定（下表）；迟到链路和数据不能改变路径 |
| NAT/代理/强制中继 | `rendezvous_mediator.rs:660` | 对称 NAT 跳过公网 IPv4 TCP；主机代理不绕行 TCP/UDP；IPv6/同网段按可用性尝试；强制中继在查询、TCP 协调和 UDP punch 三处一致隐藏直连入口，UDP 拒绝时仍返回中继地址 |
| 中继决定回传 | `rendezvous_server.rs:555`（转发 `RelayResponse`）、`rendezvous_mediator.rs:666`（对称时直接建中继） | 主机或查看者任一 symmetric、无 IPv6 且无同网段路径时，主机发 `relay_hint`；服务端只接受注册主机地址、只向同一 nonce 的发起查看者转发一次；查看者校验主机 ID 与 nonce 后立即切换中继，已确认的直连或载体会话不受影响 |
| UDP 探测 | `punch_udp` | 事务号 ACK 确认；20 ms 起乘 1.5 退避到 200 ms，最多 3 s，并受连接预算限制 |
| 连接失效 | `client/io_loop.rs:242`、`:282` | RustDesk 在 EOF/读错/30 s 无数据后结束连接；本项目直连数据路径 2 s 保活、8 s 未收到有效响应判失效（UDP 按周期检查，通常 8–10 s），auto 尝试中继，失败后由会话重连 |
| 重新连接 | `ui_session_interface.rs:1278`；`flutter/lib/models/model.dart:1064` | 销毁旧隧道，重新查询主机地址/令牌并建立新连接；旧状态不得作用于新一轮；临时查询失败延续有限重试，身份/鉴权失败停止 |

## 2. 超时和模式

`--direct-timeout=0`（默认）使用自适应算法；显式正数是 `auto` 从发起尝试起的总上限，包含协调。
默认算法中的 `punch` 为协调耗时，以下条件按顺序判断：

| 条件 | 实际连接预算 |
| --- | --- |
| 同网段或主机 symmetric | 1 s |
| 没有中继地址，或 direct/tcp 不使用中继 | 18 s |
| 双方 asymmetric，历史未失败 | 18 s |
| 双方 asymmetric，历史失败 | max(1 s, punch × 6) |
| 主机 asymmetric、查看者 symmetric | 1 s |
| 其他 | max(1 s, punch × 6)，历史失败时乘 3 |

主机 asymmetric 而本机 NAT 未知时，给后台检测最多 100 ms。
NAT 观察不会启动或重置回退计时器。UDP 探测上限 3 s；TCP 使用计算出的连接预算。
`auto` 在 TCP/UDP 直连失效后锁定中继/共享载体；`direct`/`tcp` 不回退，等待新会话重新查询；`relay`/`carrier` 强制相应路径。
CLI 默认在用户配置目录 `ds-chcast/direct-history` 保存历史，`--direct-history-dir` 可指定目录或置空禁用；
库调用者通过 `ViewerConfig.DirectHistoryDir` 启用持久化。

## 3. 控制链路和数据流

1. 主机注册后连接会合 TCP 端口，验证服务端签名挑战，并以注册私钥证明身份。
   每条连接有独立双方 nonce，20 s 保活、60 s 空闲断开，不依赖中继数据链路。
   原始 TCP 打洞端口拨号失败时，同一条控制链路改走 HTTP(S) 监听器上的
   `/chast-signal` WebSocket，挑战/证明与保活语义不变。
2. 查看者提交已认证请求。服务端签名通知含查看者观察地址、打洞 nonce、NAT 类型和同网段标志，
   并绑定当前主机控制连接。无控制连接时尝试 UDP 通知。
3. 跨网主机从协调连接端口预拨最多 30 ms，随后签名报到；同网段直接报告内网端点。
   协调连接不可达时双方改在信令 WebSocket 上报到，并附带专用于打洞的本地端口；
   服务器用观察 IP 加该端口组成对端端点。代理或公网对称 NAT 返回签名拒绝，其他路径仍可继续竞速。
4. 服务端收到报告后释放查看者。双方关闭协调连接；主机监听，查看者复用原端口拨号。
5. 查看者校验 `tcp_direct` 的主机签名与 nonce 后传输带帧边界的 QUIC 数据报。
   数据链路 8 s 空闲、2 s 保活、写入 5 s 超时；主机握手最多 10 s，查看者握手受连接预算约束。
6. UDP 直连选定后也定期探测。只有选定地址的真实数据或本轮保活 ACK 延长存活时间；
   旧事务 ACK、其他候选、会合服务消息和自身发包不计入，失效后迟到数据不得重新选中旧地址。
7. 会话重连先停止原生连接并清理旧隧道，再查询主机、创建新隧道、重新鉴权。
   仍采用 1/2/4/8/15 s、最多五次的自动重试；已有重连序列中的离线、网络错误或临时服务错误
   会继续下一次，首次启动失败不自动创建重试序列，身份、配置或鉴权失败停止。
   新原生会话启动后才轮询状态，上一轮未返回的轮询结果丢弃。

主机最多 8 个并发打洞、64 条活动 TCP 链路，nonce 保留 30 s 去重。
控制连接与配对会话各最多 1024 个；每 IP 每 10 s 最多 60 次报到/重试。
未认证查看者不获知主机存在性；主机未知 ID、错误公钥/签名统一返回鉴权失败。
连接替换、租约删除或身份改变撤销旧控制权限。relay-only 同时禁止直连报到和 UDP punch 协调。

## 4. 保留的实现差异

- 本地参考 hbbs 用 UDP 队列下发打洞指令，参考客户端另支持 TCP/WS 会合。
  本项目采用签名 TCP 会合加 UDP 备用，并在 HTTP(S) 监听器上提供
  `/chast-signal` WS 信令；没有实现相同的 protobuf 或 RustDesk 的加密控制协议，
  WS 路径、子协议与帧格式也与 hbbs 不同，不能与 RustDesk 客户端互通。
- TCP 与 UDP 分别协调并计算耗时；RustDesk 使用一个协调请求携带 UDP 端口，再建立多个连接 future。
  本项目在可打洞场景等待所有已启动尝试结束才回退，一种协调通道无响应时，总耗时可能不同；
  对称 NAT 等确定不可打洞的场景改由主机提前回传 `relay_hint`，不再等窗口走完。
- `relay_hint` 是新增控制消息，旧查看者会忽略，旧服务端不会转发；缺失时只损失提前回退，
  中继兜底和连接结果都不变，因此升级顺序没有额外要求（与其他签名控制字段不同）。
- 多中继节点时 hbbs 按请求轮询列表；本项目由中继池按区域、健康度和续租选择并下发主备地址。
  查看者仍只连当前租约，单节点失败语义与参考实现一致，但两台查看者不会像轮询那样均匀分摊。
- 查看者 TCP 阶段保留受预算约束的短间隔重拨，处理端口转为监听时的短暂拒绝；RustDesk 交给单个 connect future。
  主机侧已移除重复反拨。不能据此宣称穿透成功率完全一样。
- 数据面采用 QUIC 数据报/TCP 帧，RustDesk 使用自身流/KCP，不支持协议互通。
  既有 UDP NAT 分类与 UPnP 探测保留；本轮不是 RustDesk 全部产品功能复刻。
- RustDesk 参考实现主要在连接结束后重建会话，本项目 auto 先尝试在 QUIC 仍存活时切中继。
  8 s 数据链路失效阈值是本项目适配：本机实验中 15 s 阈值已晚于 MsQuic 的丢包断连，
  不能只依据 30 s idle timeout 决定回退时间。控制连接仍为 20/60 s。
  自动重连的五次上限也是本项目策略，并非照搬 RustDesk 的全部重试条件和时长。
- TCP 控制签名保证完整性，不提供元数据保密性。服务器与双方代理应同时升级，
  不假定新的签名报告与旧版协调服务兼容。

## 5. 回归验证

新增测试覆盖无 UDP/无 carrier 的 TCP 直连、3 s 重传前不提前回退、TCP 断链回退、
同端口预拨后报到且仅监听、对称 NAT/代理禁止直拨、nonce 去重、半帧响应跨重试、
控制连接替换/租约撤销、签名字段篡改、历史状态保存/清除、迟到候选不得改路由，
以及中继决定的来源校验、一次性转发、同网段/IPv6 存在时不提前回退与查看者读循环接线。
新增 UDP 保活 ACK 防重放、TCP 黑洞超时、主机换端口后新 Viewer 查询、启动错误分类、
有限重试及新会话启动前禁止读取旧状态测试。Windows 可选原生测试实际运行采集、编码、
MsQuic 和解码；只阻断直连，保留中继，验证回退后继续解码并恢复画面延迟。
LAN 网段规则新增 `lanmask` 的掩码解析/非法输入/同网段/空掩码单元测试，查询与
UDP 打洞的 XOR 判定、按侧下发本地中继地址、TCP 协调的内网端点替换测试，以及
启用 `--lan-mask` 后主机与查看者回环仍走直连的端到端测试。局域网候选新鲜度
新增同网段打洞时主机收到 `want_local_addrs` 后回签名 `local_addr`、查看者收到
刷新候选、伪造签名不被采信且一次性等待器在坏答案后仍可用（
`TestPunchRefreshesLANCandidates`、`TestLocalAddrAnswerMustVerify`），以及主机
代理只对带该旗标的打洞回取候选、陌生来源不响应（`TestHostAnswersLANCandidateRequest`）
的测试。反向代理用例新增
握手头保留、可信头进入中继与共享载体黑白名单判定，以及默认关闭时不采信头的
测试。注册入口新增每 ID 节流：连续 3 次后第 4 次返回 429 与 `Retry-After`、
篡改签名不消耗配额、不同 ID 互不影响、静默窗口后重置的单元测试。GeoIP
热重载新增 `Reloader` 换表/坏文件保留旧表、控制台 `rg` 输出与未配置分支，
以及热重载后登录审计地点随新表变化的 API 测试。
真实二进制回环冒烟（2026-11-12）：主机与查看者经同一 `chast-server` 实例，
`auto` 选中打洞后的 TCP 直连、`relay` 强制走上 UDP 中继、`--relay-only`
服务端让 `auto` 直接落到中继，三条路径均完成 UDP 数据往返；该冒烟只证明
路径接线和进程边界，不代表跨网穿透成功率。同日再次以真实 `chast-server` 与
`chast-tunnel host` 验证 LAN 刷新：本机来源与主机同 IP 时，查看者先收到缓存
`punch_peer`，主机随后打印 `LAN candidates refreshed` 并回签名 `local_addr`，
服务器校验后补发的第二条 `punch_peer` 携带主机当前 v4/v6 内网候选且
`is_local=true`；地址在两次保活之间真实变化的情形由
`TestPunchRefreshesLANCandidates` 覆盖。单网卡环境无法在进程级制造地址变化，
因此该冒烟只证明按请求刷新这条链路接通。
WS 信令新增 `TestSignalWebSocketPairsBothHalves`（WS 查看者与原始 TCP 主机在
同一协调器配对，查看者只报打洞端口、服务器补观察 IP）、
`TestSignalWebSocketHostControl`（控制链路与打洞共用 `/chast-signal`，
签名触发和保活均通过 WebSocket）、`TestSignalWebSocketRejectsNonSignalRequests`
（错误路径/子协议与普通 HTTP 请求被拒）、`TestPunchEndpointOverWebSocketUsesReportedPort`
（IPv4/IPv6 端口替换与不可用候选回退）、`TestDialSignalCarriesFramesOverWebSocket`、
`TestDialSignalUsesTLSForHTTPS` 与 `TestSignalEndpointDerivesHTTPAddress`
（http/https 默认端口、路径忽略与非法 scheme）。真实二进制冒烟
（2026-11-12）：服务器用 `--tcp-punch-listen` 把 TCP 协调口绑在
`127.0.0.9:18444`、只通告 `127.0.0.1:18444`，使主机的原始拨号被拒绝；
`chast-tunnel host --verbose` 打印 `rendezvous control falling back to the
signalling WebSocket`，随后一次查看者报到经该 WS 下发触发，主机打印
`TCP punch reported over the signalling WebSocket punch=0.0.0.0:<port>`，
查看者收到的对端端点正是 `127.0.0.1:<该端口>`。该冒烟同时证明控制链路和
打洞报到都只经由 HTTP 监听器，不依赖原始 TCP 打洞口。

完整结果见 `rustdesk-alignment-2026-11-08.md`。
本机 `CGO_ENABLED=0` 且缺少 gcc，未运行 Go race 检测。

## 6. 双机验收清单

### 6.1 环境

- 两台位于不同网络的机器（以下称 A=查看者、B=主机），一台有公网地址的
  服务器（或至少 A/B 都能访问）；
- 服务器放行：HTTP 端口、UDP 会合端口、**同一端口号的 TCP**（TCP 打洞
  协调）、UDP 中继端口范围、TCP 载体端口；HTTP(S) 端口同时承载
  `/chast-signal` WebSocket 信令，前置反代必须转发该路径的 `Upgrade`；
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
若服务器与部分主机同处一个内网、查看者在公网，加
`--lan-mask <内网CIDR> --local-ip <服务器内网地址>`：LAN/WAN 混合的配对直接
判为中继，LAN 侧拿到内网中继地址，避免依赖网关 hairpin。

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
| 1 | A/B 均为锥形 NAT | `--mode auto` | UDP 或 TCP 直连 | `path=direct`；会话期间中继字节为 0 |
| 2 | A/B 同一局域网，路由器不支持 hairpin | `--mode auto` | 同网段直连 | 主机日志含 `punching the viewer on the local network`；`candidate` 为内网地址 |
| 3 | A 在对称 NAT（如手机热点） | `--mode auto` | IPv6/同网段直连，或按预算中继 | 主机日志含 `symmetric NAT; skipping the v4 punch`；协调和连接分开计时，不把协调等待计入 1 s 连接预算 |
| 4 | 服务器/主机放行 TCP 但丢弃全部 UDP | `--mode tcp` | TCP 直连 | `transport=tcp`；分别记录协调和连接耗时；`auto` 默认使用自适应预算 |
| 5 | UDP 与 TCP 均不可直连 | `--mode auto` | 中继 | `path=relay`；中继字节 >0；切换时间符合预算 |
| 6 | 双方有全局 IPv6 且防火墙放行 | `--mode direct` | IPv6 直连 | 查看者/主机候选含 v6；日志 `punching the viewer over IPv6` |
| 7 | 服务器加 `--relay-only` | `--mode tcp` / `auto` | `tcp` 启动报错；`auto` 走载体/中继 | 查询无 `tcp_punch`，无直连候选 |
| 8 | 同主机两个查看者 | 两个 `--mode tcp` 并发 | 两条 TCP 链路并存 | 关闭其中一个，另一个帧流不中断 |
| 9 | TCP 链路中途断开（禁用网卡/拔线） | `--mode auto` | 回退中继 | 日志 `TCP direct path was lost; using relay`；会话不中断 |
| 10 | 目录/令牌/防火墙变更 | `--mode tcp` 并删除服务器 TCP 放行 | 失败并回退 | `auto` 在预算内回退中继；`tcp` 协调/连接失败后保持无中继；重新连接需重新发起会话 |
| 11 | 服务器与主机 B 同内网、查看者 A 在公网，服务器加 `--lan-mask`/`--local-ip` | `--mode auto` | 中继（LAN 侧用内网地址） | 查询 `nat_type=symmetric`、`relay`/`relays` 为服务器内网地址；主机日志含 `symmetric NAT; skipping the v4 punch`；LAN 侧不依赖 hairpin |
| 12 | 中继/共享载体 WS 经 nginx 终止 TLS，服务器或节点加 `--relay-proxy-header X-Real-IP` | `--mode relay`/`carrier` | 中继策略看到真实客户端 IP | 反代必须覆盖该头；`/stats` 的查看者地址与 `blocklist` 命中按客户端公网 IP；不开该旗标时仍是代理地址 |
| 13 | 同一主机 6 s 内连续注册 4 次（反复重启主机进程） | — | 第 4 次被节流 | `POST /v1/hosts` 返回 429 与 `Retry-After`，静默 6 s 后恢复；篡改签名返回 401 且不消耗配额 |
| 14 | 更新 `--geoip-file` 内容后在管理台执行 `rg` | 重新登录 | 热重载成功 | 控制台返回 `loaded <n> prefixes from <path>`；新登录的审计/会话地点按新表；文件损坏时返回 `reload failed` 且旧表继续生效 |
| 15 | A/B 同一局域网，主机在两次保活之间换了内网地址 | `--mode auto` | 同网段直连 | 服务器把该次打洞标为 `is_local` 并要求刷新候选；主机日志含 `LAN candidates refreshed`；查看者收到的 `punch_peer` 内网候选为当前地址，不是上一次保活的值 |
| 16 | 防火墙只放行 HTTP(S)，原始 TCP 协调口被拒绝或黑洞 | `--mode tcp` | TCP 直连经 WS 信令 | 主机日志含 `rendezvous control falling back to the signalling WebSocket` 与 `TCP punch reported over the signalling WebSocket`；服务器日志含 `signalling WebSocket connected`；查看者的对端端点为“观察 IP + 主机上报的打洞端口”，不是 HTTP 连接端口；原始 TCP 口可用时不出现该回退 |

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
- 回退用例：记录从故障到切换及首个恢复帧的时间，验证画面/输入恢复；若 QUIC 已断开，
  必须走新会话查询并重新鉴权，不要求所有网络下原 QUIC 连接都能继续；
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

## 8. 服务端逐能力对照（hbbs/hbbr → chast-server）

本节是 2026-11-12 的第二次复核。上一版只列出 `-m` 与中继轮询两项差异，结论
不完整；本版按 hbbs/hbbr 的每个消息分支和策略项重新逐条对照，并单独列出仍
存在的缺口。参照固定为 `D:/ref/rustdesk-server`（commit `873230e`）的
`rendezvous_server.rs`、`relay_server.rs`、`peer.rs`；只以服务端代码为准，
RustDesk 客户端行为不算本服务已实现。

判定分四档：**一致**（同语义）、**等价**（结构不同，客户端可观察结果相同）、
**增强**（本服务多出的能力，不构成缺口）、**缺口**（RustDesk 有而本服务没有，
或语义不同且会改变结果）。行号指参照实现。

### 8.1 会合、身份与在线状态

| 能力 | RustDesk 依据 | chast-server | 判定 |
| --- | --- | --- | --- |
| 主机注册与保活 | `RegisterPeer`（`:374`）；`REG_TIMEOUT` 30 s（`:50`） | 签名 keepalive 挑战/应答（`rendezvous/rendezvous.go:344`、`:368`），注册库 TTL 与 `--offline-after` | 等价（签名更强） |
| ID 与公钥绑定 | `RegisterPk`，uuid/pk/ip 一致性（`:390`） | `POST /v1/hosts` + Ed25519 注册签名（`api/api.go:552`），公钥变更返回 403 | 等价（更强） |
| 设备实例绑定 | 同 ID 换 uuid 拒绝（`UUID_MISMATCH`） | 用 `old_id` + 旧密钥迁移签名代替（`identity/identity.go:134`）；不采集机器 uuid | 增强（旧密钥不可伪造） |
| 每 IP 注册限速 | `check_ip_blocker`：30 次/60 s、300 个 ID/24 h（`:923`，`peer.rs:21-24`） | `--register-max 30`、`--register-window 1m`、`--register-max-ids 300`、`--register-id-window 24h`（`cmd/chast-server/main.go:180`） | 一致 |
| 每 ID 注册节流 | 同一 ID 6 s 内连续 3 次后第 4 次返回 `TOO_FREQUENT`（`:433`） | `--register-id-throttle 3`、`--register-id-throttle-window 6s`：签名校验通过后按 ID 计数，超限返回 429 + `Retry-After`；未认证请求不消耗配额（`abuse/idwindow.go:79`、`api/api.go:632`） | 一致 |
| 地址变更审计 | `IP_CHANGES` 180 s 窗口、360 s 保留（`:447`，`peer.rs:21-22`） | `ipchange` 默认 180 s/360 s，只记录窗口内出现多个地址的 ID（`ipchange/ipchange.go:14`） | 一致 |
| 在线状态与过期 | 30 s 未注册即离线（`:750`） | TTL + `offline-after`；共享载体或控制链路连接也可证明在线（`api/api.go:859`） | 等价 |
| 批量在线查询 | `OnlineRequest` 位图，最多 8 位/字节（`:825`） | `GET /v1/hosts?ids=`，一次 200 个 ID，未知 ID 单列 `missing`（`api/api.go:792`） | 等价 |
| 对等记录持久化 | 内存 PeerMap + database | SQLite/PostgreSQL `peers` + 服务器签名 binding（`api/api.go:1484`） | 增强 |
| 身份重置与迁移 | 换 pk 或换 uuid 拒绝 | 旧密钥签名的迁移；运营台可重置身份并撤销控制权 | 增强 |

### 8.2 打洞协调

| 能力 | RustDesk 依据 | chast-server | 判定 |
| --- | --- | --- | --- |
| 查看者发起打洞 | `PunchHoleRequest` 走 TCP/WS（`:722`） | UDP `punch_request`（`agent/viewer.go:905`）+ TCP `tcp_punch` 双向报到（`rendezvous/tcppunch.go:216`） | 等价（多一条 UDP 信令） |
| 打洞鉴权 | 共享 `licence_key`（`:726`） | 传输凭据/签名 punch auth/载体令牌，按来源校验并带重放缓存（`rendezvous/rendezvous.go:435`） | 增强 |
| 打洞限速 | 无每 IP 限制 | 30 次/10 s，控制面与 NAT 探测分别限速 | 增强 |
| 目标离线判定 | 注册时间超过 30 s（`:750`） | `registry.Online` + 控制链路/共享载体证明 | 一致 |
| 同网段判定 | `same_intranet = !ws && (peer_is_lan && is_lan \|\| 同 IP)`（`:778`） | `lanmask.Mask.SameIntranet`：双方地址都在 `--lan-mask` 网段内，或观察 IP 相同（`lanmask/lanmask.go:92`、`rendezvous/rendezvous.go:493`）；UDP 转发内网候选、TCP 内网端点替换与查询 `is_local` 用同一判定 | 一致 |
| LAN/WAN 混合强制中继 | 一端在 `-m` 网段、另一端不在时置 `SYMMETRIC` 并给中继（`:768`） | 查询/UDP 打洞/TCP 协调三处做 XOR 判定，把交换的 NAT 类型改写为 `symmetric` 并收起直连候选；主机按 `symmetric` 跳过 v4 打洞并发 `relay_hint`（`rendezvous/rendezvous.go:470`、`:594`；`api/api.go:877`；`rendezvous/tcppunch.go:375`） | 一致 |
| 局域网候选交换 | `FetchLocalAddr` 按请求向主机取最新内网地址（`:787`） | 同网段判定成立时，发给主机的 `punch_peer` 带 `want_local_addrs`，主机立即回签名 `local_addr`（主机 ID + 打洞 nonce + 完整列表 + 时间戳，`identity.LocalAddrMessage`）；服务器校验签名与来源后把刷新候选重发给同一查看者（`wire/wire.go:47`、`:88`；`rendezvous/rendezvous.go:745`、`:887`；`agent/host.go:1448`）。不额外增加请求/响应回合：旧回复已先经缓存发出，刷新只是补充 | 一致（无额外回合） |
| 向主机下发打洞 | `PunchHole`（`:804`） | `punch_peer`，带查看者 v4/v6/内网/UPnP/NAT（`rendezvous/rendezvous.go:627`） | 一致 |
| 回程确认 | `PunchHoleSent` | `punch_probe`/`punch_ack` 事务号，20→200 ms 退避、3 s 窗口，只认回程（`agent/viewer.go:1022`） | 一致 |
| 主机判定必须中继 | `RelayResponse` 经 hbbs 转给查看者（`:555`） | `relay_hint` 一次性、来源校验（`rendezvous/rendezvous.go:753`；`agent/host.go:1422`） | 等价 |
| UDP `PunchHoleRequest` | 明确不支持（`:477`） | 支持，并做完整鉴权 | 增强 |
| TCP 直连协调 | 无独立协调消息，客户端各自 TCP 打洞 | `--tcp-punch-listen` 双向签名 announce、主机签名拒绝（`rendezvous/tcppunch.go:284`、`:431`），`--relay-only` 时拒绝 | 增强 |
| NAT 类型测试 | `TestNatRequest` 在 21115 返回观察端口（`:590`、`:1158`） | UDP `nat_probe`：同一套接字看会合与中继两个目的端口，直接区分对称/非对称（`rendezvous/rendezvous.go:323`） | 等价（采样方式不同） |
| 对称 NAT 决策 | 主机按请求里的 `nat_type` 决定中继 | 主机按自身/对方 `NATType` 跳过 v4 打洞并回 `relay_hint`；查看者按 NAT 组合缩短窗口（`agent/connection_policy.go:25`） | 等价 |

### 8.3 直连通道

| 能力 | RustDesk 依据 | chast-server | 判定 |
| --- | --- | --- | --- |
| IPv4 打洞直连 | 双方 `punch_udp` | 双方 probe/ack，确认后才切换直连 | 一致 |
| IPv6 直连 | 本参照版本未注册 IPv6 | `self_v6`/`candidate_v6`/`local_addrs_v6`，v4 信令也能交换 v6 端点 | 增强 |
| UPnP | 本参照版本未使用 `upnp_port` | 代理申请映射、续租，服务端把 `upnp_port` 随保活/打洞/查询转发，主机拨映射端点 | 增强 |
| 自适应直连窗口 | `client.rs` 的 connect 分支顺序 | `directConnectTimeout` 同分支顺序（`agent/connection_policy.go:25`） | 一致 |
| 直连失败回退 | 回退中继 | 回退中继或共享载体，带直连历史 | 增强 |
| 直连历史 | 无 | 按服务器+对端+身份记录失败，下一次缩短直连预算 | 增强 |

### 8.4 中继

| 能力 | RustDesk 依据 | chast-server | 判定 |
| --- | --- | --- | --- |
| 中继协商 | 查看者 `RequestRelay`，hbbs 转给主机，主机 `RelayResponse`（`:541`、`:555`） | 查询直接返回中继地址与传输票据；主机用签名 `relay_hello` 占用租约端口（`relay/relay.go:1114`） | 等价 |
| 主机公钥证明 | `RelayResponse` 带 `get_pk` 签名 | 查询带主机公钥 + 服务器签名 binding | 一致 |
| 主机侧中继地址 | `PunchHole.relay_server` | 注册响应 `Relay`/`Relays`（本机租约或独立节点） | 等价 |
| 查看者侧中继地址 | `RelayResponse.relay_server`，LAN 时替换 | 查询 `Relay`/`Relays`；配置 `--lan-mask` 时 LAN 侧拿到 `--local-ip`（含注册响应与 IPv6/备用列表），WAN 侧保持对外地址（`api/api.go:1277`、`:1339`） | 一致 |
| 配对键 | uuid（`relay_server.rs:470`） | 每主机专属端口 + 主机签名占用，不需要 uuid | 等价 |
| 多查看者 | 每个 uuid 一对连接并发 | 主机声明 `Multiplex` 后按 mux id 隔离，上限 `carrier.MaxMuxPeers`；未声明时第二个查看者收 `busy` | 等价（旧主机降级） |
| 中继鉴权 | `-k` 校验 `RequestRelay.licence_key`（`relay_server.rs:466`） | `relay_viewer` 传输凭据/载体令牌，失败不回退（`relay/relay.go:1373`） | 增强 |
| 中继承载 | TCP + WS，WS 端口 = 中继端口 + 2（`relay_server.rs:88`） | UDP 中继端口 + TCP twin + WS（`relay/relay.go:1385`）+ 共享载体 | 等价（+UDP） |
| WS 反向代理客户端 IP | 信任 `X-Real-IP`/`X-Forwarded-For`（`relay_server.rs:430`） | `--relay-proxy-header`（默认关闭）在配置后取该头（支持逗号列表）作为查看者 IP，中继端口与共享载体 WS 握手一致；只替换 IP，端口与传输凭据重放绑定不变（`relay/relay.go:340`、`:1436`；`relay/shared.go:255`） | 一致（默认更安全） |
| 空闲回收 | 30 s 无数据关闭（`relay_server.rs:566`） | 主机 90 s、查看者 60 s | 有意差异 |
| 带宽限制 | single 128 MiB/s、total 1 GiB/s、黑名单 32 MiB/s（`:42-46`） | `policy.go` 同默认值，可按会话/总量/黑名单计费 | 一致 |
| 持续会话降级 | 1800 s 后平均速率 > 0.66×single 即降级 | 同阈值与检查窗口 | 一致 |
| 黑白名单 | `blacklist.txt`/`blocklist.txt`，每秒复查 | 同文件与复查周期，可落盘、可在控制台增删 | 一致 |
| 用量统计 | 控制台 `u` | 控制台 `u` | 一致 |
| 无空闲端口 | 单监听配对，无此失败模式 | 池耗尽时降级为“仅直连”注册（200，响应不带中继/载体，`chast_relay_lease_missing_total` 计数）；`--relay-only` 无直连兜底，返回 503（2026-11-12） | 等价（不再让主机整体下线） |
| 中继健康 | `check_relay_servers` 每 3 s 探测并剔除不可达项（`:1316`） | `--relay-nodes` 每 10 s 健康探测，失败切节点或回退本机（`api/api.go:1546`） | 等价/增强 |
| 中继选择 | `get_relay_server` 按请求轮询（`:959`） | 中继池按区域/健康选节点，主机续租保持粘性；`--relay-servers` 是同一中继的多个入口名 | 有意差异（连接更稳，但负载分摊方式不同） |

### 8.5 控制面承载与网络边界

| 能力 | RustDesk 依据 | chast-server | 判定 |
| --- | --- | --- | --- |
| 会合信令承载 | 原始 TCP 21116 + WS 端口 21118（`:134-135`、`:179`） | 原始 TCP 打洞协调口 + HTTP(S) 监听器上的 `/chast-signal` WebSocket（子协议 `chast-signal.v1`，沿用同一套带帧 `wire.Message`）；主机与查看者在原始拨号失败时改走 WS，报到只携带打洞端口，服务器把观察 IP 与该端口组合为对端端点，同网段仍用完整内网端点（`rendezvous/ws.go:20`、`rendezvous/tcppunch.go:461`；`agent/signalws.go:47`、`agent/rendezvous_control.go:34`）。WS 与控制链路共用同一协调器，鉴权/保活/触发校验不变 | 一致（承载不同，客户端可观察结果相同） |
| 中继承载 | 原始 TCP + WS（端口 + 2） | UDP 中继端口 + TCP/WS 载体 + 共享载体 | 等价 |
| 内建 TLS/证书 | 无（依赖前置代理） | 内建 HTTPS、mTLS、ACME；载体 WS 也可放在 TLS 反代后 | 增强 |
| 出站代理 | 客户端自行处理 | 代理支持 socks5/http，服务端不参与 | 增强 |
| 代理后的客户端 IP | 仅 WS 口信任代理头 | `--proxy-header` 作用于 HTTP API；中继/载体 WS 另用默认关闭的 `--relay-proxy-header`（`cmd/chast-server/main.go:157`、`cmd/chast-relay/main.go:61`），未配置时一律用 socket 地址 | 一致 |

### 8.6 运维、配置与通告

| 能力 | RustDesk 依据 | chast-server | 判定 |
| --- | --- | --- | --- |
| 本地控制台 | loopback 控制台，命令 h/rs/rg/ib/ic/pr/aur/tg（`:972`） | 127.0.0.1:8446 + token，命令覆盖上述并多出 hosts/peers/alias/tags/rv/ab/audit/connections/transfers 与中继策略命令（`admin/admin.go:229`） | 增强 |
| GeoIP 热重载 | `reload-geo(rg)` | `--geoip-file` 由 `geoip.Reloader` 持有，控制台 `rg` 重新解析同一路径并原子替换，失败保留旧表；之后的登录地点与中继区域选择立即使用新表（`geoip/reloader.go:52`、`admin/admin.go:719`） | 一致 |
| 配置推送 | loopback `ConfigureUpdate`，serial 更高才接受（`:483`） | 注册/查询每次都带 serial + 列表；`rv` 可在运行时改 serial/列表（`admin/admin.go:699`） | 等价 |
| 强制中继 | `ALWAYS_USE_RELAY` + 控制台 `aur` | `--relay-only` 原子开关，查询/TCP 协调/UDP 打洞三处一致，控制台 `aur` | 一致 |
| 软件更新 | `SoftwareUpdate` 请求/响应（`:505`） | 注册/查询携带 software 块，可托管文件、附 SHA-256/大小/双重签名 | 增强 |
| 运营键 | `-k` 共享密钥 | server-key/link-key/host-key 三层 + 传输凭据 + 访问票据 | 增强 |
| 启动自检 | `test_hbbs` 用回环探测会合端口（`:212`） | `--test-punch auto` 启动时探测本地打洞端点 | 等价 |
| 指标与 tracing | 无 | `/metrics`、按主机的 trace 关联会合与中继 | 增强 |

### 8.7 真实缺口清单（按影响排序）

无。原第 1 项“会合信令没有 WS 承载”已于 2026-11-12 关闭：HTTP(S) 监听器在
`/chast-signal` 提供 WebSocket 信令，控制链路与打洞报到都可在原始 TCP 口不可达
时改走该通道，鉴权、保活、触发签名校验与配对语义不变。
保留的差异只在行为细节：原始 TCP 口被静默丢弃（不是拒绝）时，首个原始拨号会
先等到 5 s（控制链路）或打洞窗口（查看者）才回退，这段等待仍在既有预算内。

### 8.8 结论

- 注册、查询、UDP/TCP 打洞、回程确认、LAN 网段判定与 LAN/WAN 强制中继、
  主机 relay 决策、中继配对、反向代理下的客户端 IP、鉴权、限速/名单/降级、
  配置推送和软件更新这些主路径均已与参照实现同效果；多查看者中继由 mux
  覆盖，不构成缺口。
- 上一版列出的 6 项差异已全部关闭：第 1 项 LAN 网段/强制中继（2026-11-12，
  `--lan-mask` 与 `--local-ip`，查询、UDP 打洞、TCP 协调和两侧中继地址均按
  掩码判定）、第 2 项中继/载体 WS 代理头（`--relay-proxy-header`，默认关闭，
  只替换策略看到的 IP）、每 ID 注册节流（`--register-id-throttle`，默认 3 次/
  6 s，签名通过后计数）、GeoIP 热重载（控制台 `rg` 重新解析 `--geoip-file`，
  失败保留旧表）、`FetchLocalAddr` 的新鲜度差异（同网段打洞时以
  `want_local_addrs` 附带请求，主机回签名 `local_addr`，服务器校验后把刷新
  候选补发给查看者，不增加额外回合），以及最后一项 WS 信令（`/chast-signal`
  子协议 `chast-signal.v1`，主机/查看者仅在原始拨号失败时回退，服务器用观察 IP
  加报到端口组成对端端点）。
- 有意保留的差异：按租约粘性分配中继（对端更稳，但不像轮询那样均摊）、
  批量查询代替在线位图、UDP 双观察点代替 TCP `TestNatRequest`、中继空闲
  阈值 60/90 s、以旧密钥迁移签名代替设备 uuid、用 `relay_hint` 代替
  `RelayResponse` 的主机决策回传。这些不改变连接结果。
- 关闭顺序已完成，本表当前没有未关闭的“缺口”行；新增差异仍按同一格式记入
  8.7，并在关闭时补 loopback 与单元测试。
