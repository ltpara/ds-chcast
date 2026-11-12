# 发射端审核问题修复记录

日期：2026-11-12。对应 [完整审核](D:/ds-chcast/docs/sender-audit-rustdesk-2026-11-11.md)。范围包括主机生产路径及完成协议闭环必需的查看端改动。工作区还有其他编辑，本文只说明此次审核问题的修复和实际验证。

## 编码策略约束

**保持无限 P 帧、按需 IDR。** `EncoderConfig::gop_size` 和主机编码配置保持 0，默认 `HostServerConfig::keyframe_interval_ms` 保持 0。没有引入固定 GOP，也没有在主机或查看端录像期间每 8 秒请求 IDR。

初始审核 A02 将 RustDesk 的录制周期关键帧当成我方必须补齐的能力，这一判断已撤回。RustDesk 录制时使用 240 帧关键帧间隔，是设计差异。录制启动、新文件/分段、队列溢出后的引用恢复等事件仍可按需请求 IDR。已有 10 分钟分段是文件边界需求；持续录像本身不新增周期请求。

新增回归检查主机在 8 秒与分段前不请求 IDR、分段时按需请求，以及查看端录制静态画面超过 8 秒不新增请求。长 GOP 的随机定位开销仍需要实际测量，不通过修改常态编码策略解决。

## 已修复的问题与效果

| 问题 | 实现 | 已有验证 |
|---|---|---|
| A01：光标采样等待视频 tick 或慢编码 | 为桌面 Source 增加约 33 ms 的独立指针线程，Win32 采样不依赖 D3D；切换输入桌面后取位置和形状，再发布到订阅者。多流路由变化重置发送缓存，补发完整形状。采样位置保留 OS 热点，查看端按 sprite 热点绘制 | 5/30 FPS 与注入 300 ms 编码阻塞的确定性用例；真实 QUIC 多屏专项覆盖路由切换、视频积压、移除与重新加入 |
| A03：大截图占用控制流、流 0 与“当前流”混用 | 每个截图响应走独立单向 `CSHT` 流，截图未完成发送限制 512 KiB；PNG 编码在辅助线程，控制会话轮询结果；用 `UINT32_MAX` 表示当前流，0 表示明确的流 0 | 2 MiB 响应分片经真实 QUIC 收齐；首片之后控制消息在截图流仍打开时到达；生产主机原始 PNG 截图通过 |
| A04：软件编码仍依赖 GPU 采集和转换 | 增加 `GdiCpu`，DIB 输出不可变 BGRA；VP9 通过 libyuv 消费 CPU 像素（OpenH264 已于 2026-11-12 移除，CPU 采集只允许 VP9）。独立桌面捕获在设备/转换失败时可进入 CPU 路径；慢端压缩队列不保留每帧原始 BGRA | 无 D3D 设备的 VP9 4:2:0/4:4:4 编码、首帧与按需 IDR；本机真实 GDI CPU→共享采集→软件 VP9→截图 |
| A05：未认证连接挤占正式观看者 | 拆分传输上限与认证后配额，认证、人工同意通过后才占正式名额；即使正式上限为 1 也使用独立会话工作线程。满额拒绝不提交临时密码消费/信任状态 | 正式上限 1 和 2 下，分别建立对应数量的 TLS 完成但不发送 Hello 的连接，合法文件会话仍可进入；正式满额及拒绝记录回归通过 |
| 弱网验收门槛过宽 | 峰值帧龄门槛由 15 秒收紧到 3 秒。关闭 QUIC 内部发送缓冲，使应用发送预算根据实际传输完成释放，防止截图预算只限制内部复制 | 下述真实硬编与 netem 回归通过；没有把严格门槛本身当成延迟性能提升 |

关键代码：

- [独立指针与截图读取](D:/ds-chcast/src/host/desktop_stream.cc)、[Win32 指针采样](D:/ds-chcast/src/capture/cursor.cc)。
- [主机会话、配额及异步 PNG](D:/ds-chcast/src/host/host_server.cc)、[QUIC 截图流及认证配额](D:/ds-chcast/src/net/quic.cc)。
- [GDI CPU 捕获](D:/ds-chcast/src/capture/gdi_capturer.cc)、[共享捕获回退](D:/ds-chcast/src/capture/shared_capturer.cc)、[VP9 软件编码](D:/ds-chcast/src/codec/vpx_encoder.cc)（OpenH264 已于 2026-11-12 移除）。

## 验证结果

Release 构建后执行 10 项 CTest，**10/10 通过，254.37 秒**：`protocol`、`host_multi`、`host_parity`、`desktop_stream`、`display_subscriptions`、`display_video`、`viewer_display_streams`、`host_control`、`mp4_recorder`、`quic_weak_network`。

随后修正独立采样的热点坐标，并加入最后一个观看者退出时仍有截图任务的生命周期回归。重新构建后，多屏光标专项通过；`cursor`、`protocol`、`desktop_stream` **3/3 通过，15.29 秒**，其中截图退出场景通过。最终 `chast-host` 与 `chast-service` Release 构建成功。

| 测量 | 结果 | 口径 |
|---|---|---|
| 5 FPS 指针回调 | 34.16 ms；编码阻塞时 35.07 ms | 最后补充回归：注入采样器状态变化→生产订阅回调；不是双机呈现延迟 |
| 30 FPS 指针回调 | 35.52 ms；编码阻塞时 1.99 ms | 同上，各项低于 150 ms 断言门槛；不是统计分布 |
| 弱网完整性 | 392 发送 / 392 接收 / 392 解码；12 个关键帧 | 本机真实 Intel 编码；恢复和订阅等事件允许按需 IDR |
| 弱网峰值帧龄 | 2446 ms，通过 `<3000 ms` 门槛 | 单向基础延迟 80 ms、抖动 0–20 ms、3% 丢包、1 Mbps，4–12 秒损伤 |
| 弱网策略变化 | 最低 1000 kbps，恢复到 1672 kbps；14 次调整，末尾 30 FPS、几何保持 | 不能等同 capture-to-display 或 input-to-photon |

构建和测试材料保存在本地忽略的 build 目录：

- [回归构建日志](D:/ds-chcast/build/fix-sender-build-verified.log)。
- [10 项回归汇总](D:/ds-chcast/build/fix-sender-tests-verified.log)、[完整输出快照](D:/ds-chcast/build/fix-sender-tests-verified-details.log)。
- [最终多屏光标专项输出](D:/ds-chcast/build/fix-sender-cursors-postreview.log)。
- [补充回归](D:/ds-chcast/build/fix-sender-postreview-tests.log)、[完整输出](D:/ds-chcast/build/fix-sender-postreview-details.log)、[补充构建](D:/ds-chcast/build/fix-sender-build-postreview.log)。
- [交付程序构建日志](D:/ds-chcast/build/fix-sender-build-delivery.log)。

## 协议与尚未覆盖的边界

截图传输和“当前流”语义已改变，协议版本升至 **72**，主机与查看端需要配套更新。`CSHT` 拆流消除了截图与控制消息在同一可靠流上的按序等待，但仍共享连接带宽，不代表绝对优先级保障。

GPU 截图读回仍在显示源线程串行执行，可能短暂停顿视频；PNG 编码已不占会话线程。CPU 兜底覆盖独立桌面路径；外置设备的合并多屏采集仍要求 GPU。没有通过禁用真实驱动来验证全部失败分支，也没有完成 SYSTEM/UAC/锁屏、多适配器和真实双机时延验收。

共享 QoS 仍采用全局保守策略，一个慢端可以降低其他端质量/FPS。这是已有资源取舍，本次没有改成每个观看者独立编码策略。预认证传输仍有总上限（正式配额加 8），本次名额隔离不等于消除所有资源耗尽可能。

RustDesk 对照基线为本机 `D:/rustdesk` 的 `7423dced37845c48093b6f71bd82e4a16cc8c79d`，只读源码，未运行二进制。以上结果不能支持“整体画质/延迟胜过 RustDesk”的结论；仍需同内容、同实际码率和同硬件 A/B。2026-11-12 复核：`D:/rustdesk-ref` 路径已不存在，`7423dce` 仍可从 `D:/rustdesk` 按内容检出，但当前本机 HEAD 是 `55786aa`；本轮审计以 `55786aa` 为准，见 [发射端审计](sender-audit-2026-11-12.md)。

P3 后续（同日，工作区）：协议升至 **v73**，新增 `ControlType::CursorShapeRequest`（88），查看端可在形状缓存未命中时按（流、形状 ID）重取，主机从最近一次 sprite 缓存重发。旋转截图的 CPU 降级做了实现与几何单测，但复核发现旋转帧不会进入截图缓存、视频流会先失败，截图层降级在生产路径不可达，代码已回退；结论与后续方向见 [发射端审计](sender-audit-2026-11-12.md) 9.2。
