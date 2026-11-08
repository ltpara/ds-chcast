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
| 同网段 | `FetchLocalAddr` / intranet | 双方观察 IP 相同才返回主机签名的内网 TCP 端点，免公网预拨，连接预算 1 s；UDP 保留 LAN 候选 |
| 请求重试 | `client.rs:449` | 同一 TCP 连接、同一 nonce，分别等待 3/6/9 s；重试只通知主机，不新建等待会话；断开清理；读取响应不因重试丢失半帧 |
| 连接预算 | `client.rs:649` | 按下表实现分支顺序，协调时间不提前耗尽实际连接预算 |
| 历史结果 | `client.rs:210`、`:2063` | CLI 按服务器/主机 ID/公钥保存上次直连失败，成功清零，回退置一 |
| 路径竞速 | `client.rs:684` | TCP、UDP/IPv6 并行；选定后取消其他尝试；默认全部结束才回退；迟到链路和数据不能改变路径 |
| NAT/代理/强制中继 | `rendezvous_mediator.rs:660` | 对称 NAT 跳过公网 IPv4 TCP；主机代理不绕行 TCP/UDP；IPv6/同网段按可用性尝试；强制中继隐藏直连入口 |
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
2. 查看者提交已认证请求。服务端签名通知含查看者观察地址、打洞 nonce、NAT 类型和同网段标志，
   并绑定当前主机控制连接。无控制连接时尝试 UDP 通知。
3. 跨网主机从协调连接端口预拨最多 30 ms，随后签名报到；同网段直接报告内网端点。
   代理或公网对称 NAT 返回签名拒绝，其他路径仍可继续竞速。
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
连接替换、租约删除或身份改变撤销旧控制权限。relay-only 禁止直连报到。

## 4. 保留的实现差异

- 本地参考 hbbs 用 UDP 队列下发打洞指令，参考客户端另支持 TCP/WS 会合。
  本项目采用签名 TCP 会合加 UDP 备用，没有实现相同的 protobuf、加密控制协议或 WS 会合。
- TCP 与 UDP 分别协调并计算耗时；RustDesk 使用一个协调请求携带 UDP 端口，再建立多个连接 future。
  本项目等待所有已启动尝试结束才回退，一种协调通道无响应时，总耗时可能不同。
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
控制连接替换/租约撤销、签名字段篡改、历史状态保存/清除，以及迟到候选不得改路由。
新增 UDP 保活 ACK 防重放、TCP 黑洞超时、主机换端口后新 Viewer 查询、启动错误分类、
有限重试及新会话启动前禁止读取旧状态测试。Windows 可选原生测试实际运行采集、编码、
MsQuic 和解码；只阻断直连，保留中继，验证回退后继续解码并恢复画面延迟。

完整结果见 `rustdesk-alignment-2026-11-08.md`。
本机 `CGO_ENABLED=0` 且缺少 gcc，未运行 Go race 检测。

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
