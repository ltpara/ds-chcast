# 传输兜底边界：agent 会话内回退与查看端重连回退（2026-11-14）

## 1. 结论

审核问题“TCP 兜底不完整的断网恢复”**部分成立**：

- 所述“UDP 收到首个数据后监控线程退出”的代码在已提交树中不存在。
- 主机 agent（Go）已实现会话内回退：直连/TCP 失效后就地切到 carrier/relay，
  且迟到的新链路不能把会话改回直连。
- 原生查看端（C++）只在**连接阶段**依次尝试 QUIC → TCP carrier → WS carrier；
  会话中 UDP 通道整体失效时，靠会话断开 + 查看端自动重连恢复，会重新走一遍
  连接尝试序列，因此可落到 carrier，但带一次可见中断和重新认证，不是无缝切换。

## 2. 主机 agent（Go）：会话内回退已实现并有测试

源码路径：

- `server/internal/agent/viewer.go:1606`：迟到的 UDP 探测不得把 TCP 或回退会话
  改回直连。
- `server/internal/agent/viewer.go:1642`：回退锁存后，迟到的直连包不能切换回去；
  `attachTCP` 在锁存后拒绝新 TCP 链路。

本轮实际运行的定向测试（全部通过，1.675 s）：

```powershell
cd server
go test ./internal/agent -run 'TestCarrierOnlyTCPTimeoutFallsBackBeforeLocalClient|TestCarrierOnlyTCPLossFallsBackAndStaysOnCarrier|TestAttachTCPDoesNotUndoRelayFallback|TestAutoFallbackDoesNotFlap|TestPathInfoReportsInitialPathAndFallback|TestRelayOnlyForcesRelayAndRefusesDirect' -count=1 -v
```

对应覆盖：

| 用例 | 位置 | 证明 |
| --- | --- | --- |
| `TestCarrierOnlyTCPTimeoutFallsBackBeforeLocalClient` | `agent_tcp_test.go:158` | TCP 打洞超时、尚无本地客户端时先落到 carrier |
| `TestCarrierOnlyTCPLossFallsBackAndStaysOnCarrier` | `agent_tcp_test.go:192` | 会话中 TCP 直连断开 → carrier，且迟到的 TCP/UDP 均不撤销 |
| `TestAttachTCPDoesNotUndoRelayFallback` | `agent_tcp_test.go:236` | relay 回退后拒绝新 TCP 链路 |
| `TestAutoFallbackDoesNotFlap` | `agent_test.go:339` | 自动回退不来回抖动 |
| `TestPathInfoReportsInitialPathAndFallback` | `agent_test.go:360` | 路径信息如实上报回退 |
| `TestRelayOnlyForcesRelayAndRefusesDirect` | `agent_carrier_test.go:402` | relay-only 模式拒绝直连 |

## 3. 原生查看端（C++）：连接期选择 + 重连恢复

- 尝试序列固定在连接阶段：`app/windows/viewer/viewer_engine.cc:3088-3093`
  （`attempts = {kQuic, kTcp, kWebSocket}`；显式指定传输时只尝试该传输）。
- TCP/WS 走主机 QUIC 端口 +1 的 carrier（或中继/端口映射指定端点），
  carrier 拨号超时自动模式 3 s、指定模式 8 s；UDP 连接超时自动模式 4 s：
  `viewer_engine.cc:3116-3145`、`3183-3192`。
- 连接成功后 carrier bridge 随会话存活；单条路径失败后推进下一条；一旦对端已经
  出示证书（TLS 栈可达），身份拒绝即终止整个尝试序列：`viewer_engine.cc:3196-3210`。
- 会话中地址变化由 QUIC 连接迁移/NAT 重绑定承接（未禁用迁移；保活间隔设置见
  `src/net/quic.cc:6113`、`6283`），但 UDP 通道整体不可用时连接终止。
- 会话断开后由 Flutter 端 `AutoReconnectPolicy` 恢复（`app/lib/reconnect_policy.dart:7`
  的 1/2/4/8/15 s 阶梯，最多 5 次），每次重连都会重新执行上述连接尝试序列，
  因此断网后可能落到 TCP/WS carrier。

边界：本端没有“会话内重新拨号”的 carrier 切换。UDP 已死而 TCP carrier 本可
承接时，用户看到的是断线重连，而不是无缝切换；这依赖主机 agent 侧的回退为
查看端兜底（直连失效后主机自身仍在线并可经 carrier/relay 接受新连接）。

## 4. 本轮不改 `viewer_engine.cc` 的原因与后续建议

`app/windows/viewer/*`（含 `viewer_engine.cc`）属于并行工作流负责的
viewer-engine 范围，本分支约定不编辑该目录；会话内 carrier 重拨还涉及会话
生命周期、认证票据复用与流重建设计，超出“兜底缺失”的修复范围。后续增量建议：

1. `QuicSession` 暴露可分类的连接状态（UDP 不可达 vs 证书/权限拒绝），查看端在
   会话内启动 carrier bridge 并重建客户端，保留 ticket/设备身份，实现真正会话内
   切换。
2. 或在断开前预建 carrier 备链，缩短重连窗口。
3. 双机 + 丢包/断网矩阵验收（本机无 netem、无双机环境，本轮未做）。

## 5. 验证边界

本轮完成了源码核对与 Go 定向测试；未做真实断网、丢包矩阵或双机时序验证。
查看端重连调度只由既有 `reconnect_policy` 测试覆盖，真实网络切换的恢复时延
仍待实测。