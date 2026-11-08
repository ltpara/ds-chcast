# RustDesk 连接修复与验证（2026-11-08）

本轮连接流程修复及本机验证已完成。参考本地 RustDesk 客户端 `7423dced`、服务端
`873230e3`；保留本项目 QUIC 数据报及身份协议，不要求 RustDesk 协议互通。

| 检查项 | 实现与证据 |
| --- | --- |
| 独立会合控制 | 服务端认证后通知主机；不依赖 viewer carrier 令牌/会话/配额；`TestTCPPunchWithoutCarrierWhenUDPExpired` 在关闭 UDP 且无 carrier 主机链路时仍完成 TCP 数据往返 |
| 预打洞后报到 | `TestHostPrePunchUsesReportPortAndThenOnlyListens` 检查预拨源端口等于报告端口，主机不等配对回包即转监听，之后无反拨；`TestHostControlTriggerAndHostReportOrdering` 检查服务端不提前释放查看者 |
| 协调重试和取消 | 同连接同请求等待 3/6/9 s；实际 3 s 重传测试、半帧响应跨重试测试、断开清理测试通过 |
| 自适应连接预算 | 逐项覆盖 RustDesk NAT/同网段/无中继/历史失败/最低 1 s 的分支顺序；协调阶段不会被 NAT 观察提前截断 |
| 历史结果 | CLI 默认按服务器和主机身份保存；新 Viewer 可读回，成功后清零；库配置可选启用 |
| 路径竞速与回退 | 无 UDP 候选也尝试 TCP；超时与 TCP 断链转中继/carrier；UDP 胜出取消 TCP；迟到 TCP/UDP 不改变已选路径 |
| 策略与鉴权 | 代理/对称 NAT 禁止公网 TCP 预拨；主机代理不发 UDP 保活/打洞；同网段按签名内网端点连接；强制中继拒绝直连；控制连接替换/租约删除和签名字段篡改测试通过 |
| 打洞后的存活检测 | UDP/TCP 数据路径 2 s 保活、8 s 未收包判失效；UDP 只认选中端点、本轮 ACK 或真实数据；旧 ACK 和迟到包不能延长/恢复失效路径 |
| 新会话重新查询 | Flutter 先停止原生连接和旧隧道，再刷新地址/令牌；主机重启换端口后，新 Viewer 的 TCP 数据往返测试通过 |
| 重连中的临时失败 | JSON 启动错误保留可重试标志；离线/网络错误延续已有的五次重试，身份/鉴权失败停止；新会话启动前不读取旧状态，迟到轮询结果作废 |

验证结果：

- `go test ./... -timeout=5m`：本轮通过，agent 套件 116.411 s。
- `go vet ./...`：通过。
- 前一轮关键故障与策略测试连续 3 轮通过：agent 12.435 s，rendezvous 0.333 s，identity 0.036 s。
- Flutter 隧道、重试策略、服务器 API、会话窗口与启动轮询共 122 项回归通过；相关 Dart 静态检查无问题。
- Go race 未运行：当前 `CGO_ENABLED=0` 且未找到 gcc。

本轮新增 Windows 原生验收 `TestNativeQUICAfterUDPBlackhole`，使用本机
`build/Release/chast-quic.exe` 实际采集、H.264 编码、MsQuic 传输和解码。
测试建立已选定的 UDP 直连转发路径，3 s 后丢弃该路径双向数据，保持会合服务和中继可达。
它验证打洞后的恢复，不代替真实 NAT 打洞成功率测试。最终运行结果：

- 约 9.00 s 检出并选择中继，原 QUIC 会话完成 32 s 运行；
- 收到 318 帧、解码 314 帧，CRC/解码/协议错误均为 0；
- 恢复后的最终画面延迟约 14.3 ms，故障期间最高约 9.9 s；不能称为无感切换；
- 测试还要求故障后继续解码、运行到结束、最终画面延迟低于 1 s，避免只检验回显或零错误计数。

可在交互式 Windows 桌面显式运行（普通 Go 测试会跳过采集测试）：

```powershell
$env:CHAST_QUIC_TEST_BINARY='D:\ds-chcast\build\Release\chast-quic.exe'
cd D:\ds-chcast\server
go test ./internal/agent -run '^TestNativeQUICAfterUDPBlackhole$' -v -count=1 -timeout=90s
```

与参考实现仍有适配差异：TCP/UDP 分别协调，因此某一路协调无响应时总等待可能不同；
查看者 TCP 在预算内短间隔重拨；控制协议/签名与 QUIC 数据帧是本项目实现，非 RustDesk
protobuf/加密流/KCP 协议。新的签名报告要求服务器与双方代理一起升级。

打洞后的生命周期现已遵循 RustDesk 的“监测存活、失效清理、重建连接并重新鉴权”原则
（`client/io_loop.rs:242–287`、`ui_session_interface.rs:1278`、
`flutter/lib/models/model.dart:1064–1098`）。本项目 auto 额外尝试在 QUIC 存活时切中继；
8 s 失效阈值和五次重试上限是本项目策略，并非 RustDesk 的相同参数。
15 s 失效阈值在最初原生试验中已晚于 MsQuic 丢包断连，因此调整为 8 s；
仅对照 QUIC 30 s idle timeout 不足以保证回退及时。控制连接的 20/60 s 保活/超时不变。

真实跨网 NAT、IPv6、防火墙及弱网成功率尚未验证，不能以回环测试声称与 RustDesk 完全一致。
完整对照及双机验收步骤见 [打洞与 RustDesk 对照](nat-traversal-2026-11-07.md)。
