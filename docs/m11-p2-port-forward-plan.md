# M11 P2：端口转发（chast 自有设计）

状态：协议 v26、传输、主机端、CLI、查看器与主机 GUI 全链路验收完成（2026-10-27）；
双机/中继验收与远程终端未开始。
目标：查看器把本机一个 TCP 端口映射到主机侧可达的 TCP 地址（典型用途：RDP 3389、SSH 22），
数据全程走现有 QUIC 会话，不新增监听端口。

## 1. 与 RustDesk 的差异与取舍

RustDesk 的端口转发是独立登录类型（`ConnType::PORT_FORWARD`），新版本在一条双向 QUIC 流上
复用 `PortForwardChannel` 帧（Open/Opened/Data/Close/WindowUpdate），自带应用层窗口。

chast 不照搬：

- 沿用“一条会话、控制流 + 多条数据流”的模型。端口转发不单独建连，不另设认证；
  它和画面、文件一样是已授权会话里的一项能力。
- 每个被转发的 TCP 连接使用**两条单向 QUIC 流**：查看器开上行 `viewer -> host`，主机开下行
  `host -> viewer`。两条流用同一个 `(forward_id, connection_id)` 关联。方向与既有视频/音频/
  文件流一致，QUIC 每条流自带流控，天然把 TCP 背压映射过去，不需要应用层窗口协议。
- 控制流只负责“登记/撤销转发目标”，不携带数据。

不采用双向流的理由：当前 `QuicSession` 对端发起的双向流只有控制流一条，引入多双向流需要重写
流归属判定；两条单向流可直接复用现有的 magic 分类、接收暂停和关闭路径。

## 2. 协议（v26）

版本号 25 -> 26。新增控制消息：

| 值 | 消息 | 方向 | 载荷 |
| --- | --- | --- | --- |
| 33 | PortForwardRequest | viewer -> host | forward_id u32, port u16, host_len u16, host UTF-8 |
| 34 | PortForwardResult | host -> viewer | forward_id u32, code u8, reason_len u16, reason |
| 35 | PortForwardStop | 双向 | forward_id u32, code u8, reason_len u16, reason |

`PortForwardCode`：Ok=0、Refused=1（主机策略不允许）、InvalidTarget=2、Busy=3、
Cancelled=4（任一侧撤销）。

权限位 `kPermissionPortForward = 0x0080`；`kPermissionAll` 纳入。HelloAck 的 ServerInfo
增加 `kServerInfoPortForwardEnabled = 0x0080`，`kServerInfoKnownFlags` 同步。0x0040 保持
未用：它在 HelloAck 标志字里是 merged-layout 标记，不是权限；这样权限掩码和标志字仍可逐位比较。

### CPFW 流头（16 字节）

```
magic "CPFW" (4) | version=1 (1) | reserved (3) | forward_id u32 | connection_id u32
```

- 查看器打开上行流时写头；主机校验 forward_id 已登记且权限仍在，把连接交给主机应用层拨号。
- 主机拨号成功后再打开下行流并写同样的头；查看器按 connection_id 把它接到本地 TCP 连接上。
- 后续字节就是 TCP 原始字节，无额外帧。流 FIN = 对应方向的 TCP 半关闭；流被 abort =
  整条连接失败，另一侧关闭本地/目标 socket。

### 数量上限

- 每条会话最多 8 个 forward_id、32 条并发转发连接。
- 单次发送块 64 KiB；接收队列高水位 256 KiB，超过则暂停该流的 QUIC 接收（不丢字节）。

## 3. 主机侧

- `HostSettings.allow_port_forward`，默认 **false**。设置文件键 `port_forward`；
  同样并入 `permissions=` 掩码。
- `HostServer` 在会话循环里：
  1. `take_port_forward_request`：检查设置与权限，登记 `forward_id -> {host,port}`，
     回答 `PortForwardResult`。
  2. `take_port_forward_open`：对每个新连接拨 `host:port`，成功后
     `accept_port_forward_connection` 打开下行流。
  3. 双向泵送：socket -> 上行流、下行流 -> socket，直到两侧都收到 FIN；
     TCP EOF 映射为 `finish_port_forward`，socket 错误/拨号失败映射为
     `close_port_forward`（abort），另一侧立即看到失败；会话结束或权限撤销时
     关闭全部 socket。
- 实现取舍：`PortForwardRelay` 不使用每连接线程，而是由主机会话循环
  `pump()` 驱动非阻塞 socket（`getaddrinfo` + 非阻塞 `connect` + `select`，
  拨号超时 5 s，单次 64 KiB）。这样所有 `QuicSession` 调用仍只发生在
  主机线程上，不破坏“单所有者”约定，也不会在 MsQuic 回调里阻塞。
- 审计/日志记录 `approved/dialing/connected/closed` 与失败原因；不记录数据内容。
- 风险：启用后查看器可让主机连接任意可达地址。默认关闭、按会话授权、并发上限，
  并在文档与设置说明里写清楚。

## 4. 查看器侧

- 原生 API：`request_port_forward` / `take_port_forward_result` / `open_port_forward_connection`
  / `wait_port_forward_connection` / `send_port_forward_data` / `receive_port_forward_data` /
  `close_port_forward_connection`。
- 本机监听固定绑定 `127.0.0.1`（默认），每个 accept 建一条转发连接；本地端口可配置，
  不默认暴露到局域网。
- CLI 验收：`chast-quic verify --port-forward` —— 主机真实拨号到 127.0.0.1 的临时 TCP
  echo 服务，查看器开一条转发连接、发送并校验回显；随后关闭并确认两端清理。
- GUI：会话工具栏“端口转发”对话框（本地端口、远程主机、远程端口、启停）放到下一刀，
  先保证原生链路与 CLI 验收。

## 5. 测试与验收

- 协议单测：三个控制消息的编解码、非法长度/UTF-8/端口、CPFW 头（已通过）。
- `verify --port-forward`：真实 TCP loopback（主机拨号 + echo + QUIC loopback），
  断言回显字节、连接计数、关闭后无残留（已通过，见 §6）。
- 边界：无权限时请求被拒绝（客户端 `permission_mask_from_info` 已补齐
  `port_forward_enabled`，否则 HelloAck announce 与本地闸门会不一致）；
  `verify --port-forward-denied` 已作为独立负向探针通过（见 §6 第 7 条）；
  重复 forward_id；超过并发上限；主机拨号失败时查看器本地连接被关闭；
  会话中途撤销权限。
- 不做：UDP 转发、动态 SOCKS、把转发目标持久化到磁盘。

## 6. 实施记录（2026-10-26）

1. 协议 v26（消息 33/34/35、CPFW 头）与 `QuicSession` 双单向流数据面、
   控制面已完成；`tests/protocol_test.cc` 覆盖编解码与拒绝用例。
2. 主机设置 `port_forward` / `permissions` 0x0080 与 `HostServer` 接线完成；
   `PortForwardRelay` 由会话循环驱动，不使用额外线程。
3. 生命周期修正：两侧流都 `SHUTDOWN_COMPLETE` 后条目不再立刻删除，而是
   保留到应用 `close_port_forward`，保证接收队列尾部可读、`receive` 在
   排空后返回 `Unavailable`；并发上限只计未结束条目，必要时先回收退役条目。
   主机 relay 在干净结束时也调用 `close_port_forward` 释放条目。
4. 客户端 `permission_mask_from_info` 补上 `port_forward_enabled`，
   修复“HelloAck 宣布可用、`request_port_forward` 却本地拒绝”的不一致。
5. CLI：`serve --allow-port-forward`、`verify --port-forward`。
   verify 在 127.0.0.1 启动临时 TCP echo，主机真实拨号；
   查看器推送 5×64 KiB + 123 B（327803 B），逐字节比对回显。
   实测日志：`approved -> dialing -> connected -> closed`，
   `port-forward code=0 sent=327803 received=327803 echo=1`。
6. `ctest -R "quic_port_forward|quic_loopback|quic_merged|host_settings|protocol"`
   全部通过（quic_port_forward 约 32 s）。
7. 负向探针：`verify --port-forward-denied` 主机保持 `allow_port_forward=false`，
   HelloAck 不宣布 `port_forward_enabled`；查看器 `request_port_forward` 得到
   `PermissionDenied`，之后会话继续跑完全程（151 帧照常收发）。
   ctest `quic_port_forward_denied` 通过（约 30 s）。
8. 查看器侧监听器 `PortForwardClient`（`include/chast/port_forward_client.h`）：
   绑定 127.0.0.1，主机 Ok 之后才 accept 本地 socket，每个 socket 一条
   `connection_id`，轮询式 `pump()` 与主机 relay 对称。
   探针 `verify --port-forward-local`：本地 TCP 客户端连监听器，经 CPFW
   到主机真实拨号再回显，327803 B 逐字节一致；ctest
   `quic_port_forward_local` 通过（约 32 s）。
9. 干净收尾修正：relay 在 `Done` 后调用 `close_port_forward`，过去会无条件
   `ABORT` 两条流，把尚未确认的发送尾包丢掉（曾截断到 182750/327803 B）。
   现在两侧都 `send_finished && receive_finished` 时只标记 `app_closed`，
   交给 `SHUTDOWN_COMPLETE` 关闭句柄；失败/停止路径仍立即 abort。
10. 剩余：查看器 Flutter 端口转发对话框与引擎接线、主机 GUI 设置项（均在 §7 完成）、
    双机（经中继/真实网络）验收、远程终端。

## 7. 查看器 GUI 与实机验收（2026-10-27）

1. 查看器接线：`ViewerEngine` 增加 `StartPortForward(local_port, host, port)` /
   `StopPortForward` 命令与 `PortForwardStatus` 事件（监听端口、目标、连接数、
   失败原因）；`PortForwardClient` 在引擎轮询线程里 `pump()`，会话断开时
   `stop()` 并关闭监听器。`ServerInfo` 的 `port_forward_enabled` 进入
   `ViewerStatus.acceptsPortForward`。
2. Flutter：会话工具栏 `Icons.alt_route` 按钮，对话框“本地端口 / 目标主机 /
   目标端口”，本地端口 0 表示由系统分配，启动后显示实际绑定端口；
   服务端未开放时按钮禁用并提示“服务端未开放端口转发”。
3. 实机验收路径：`chast-quic serve --port 45610 --password 12345678
   --auth-mode password --token FDE7GKJ9 --token-reuse --allow-port-forward`
   + `chast_viewer.exe`，首页输入令牌连接，会话内打开对话框
   （51660 -> 127.0.0.1:45999），本机 TCP echo 服务真实回环 1 MiB
   （64 KiB 写读交替），结果 `echoed 1048576 of 1048576 bytes: MATCH`；
   查看器日志 `accepted/forwarding/closed`，主机日志
   `approved -> dialing -> connected -> closed`，两端均无残留。
4. GUI 首轮失败暴露两个真实缺陷，均已修复：
   - **主机单向流权限闸门漏项**：`QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED`
     对服务端入站单向流只放行剪贴板/文件/麦克风，v26 新增的端口转发未纳入，
     只开 `--allow-port-forward` 的主机在 CPFW 分类前就 abort 了上行流，
     查看器看到 `the peer stopped reading the port forward stream`。
     闸门改为四类合一（`kPermissionPortForward` 纳入），CLI 回环之所以没发现，
     是因为 `verify` 的内置主机同时开启了剪贴板/文件/麦克风。
   - **接收队列越界即中止**：QUIC 接收事件必须整包接受或整包拒绝，
     原逻辑在“已缓冲 + 本事件”超过 512 KiB 时直接 abort，而慢速本地端
     造成的积压是正常背压。改为到高水位即暂停（整包不接收，
     `StreamReceiveSetEnabled(FALSE)`），排空到 128 KiB 再恢复；
     不再对合法的突发流量断开连接。浏览器/客户端“先写完再读”的测试脚本
     本身会在任何 TCP echo 上死锁，验收脚本因此改为 64 KiB 写读交替。
5. 回归：`ctest -C Release -R quic_port_forward` 三项（正向、负向闸门、
   查看器监听器）全部通过；临时诊断输出已全部移除。
6. 主机 GUI 设置（2026-10-27）：安全设置对话框“访问控制”页新增“端口转发”
   复选框与说明“允许查看器经本机访问你可达的 TCP 地址。”，托盘菜单新增
   “允许端口转发（查看器访问本机 TCP）”；权限摘要里也显示“端口转发”。
   `current_gate_mask()` 补上 `kPermissionPortForward`——缺这一位时，会话中
   任意一次实时权限切换都会把 0x0080 从会话权限里撤掉。验收：测试配置
   `build/host-gui-test.ini` 勾选保存后写入 `port_forward=1`、`permissions=143`
   （0x8F 含 0x0080），重开对话框保持勾选；首版把复选框放进了“接收目录”那一行，
   被静态文本覆盖，已改为独立一行。未改动真实 `%APPDATA%\chast\host.ini`。
