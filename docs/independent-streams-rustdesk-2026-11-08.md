# 单连接多屏订阅：实现进度与 RustDesk 对照

目标是完整复刻单条连接同时订阅多个独立显示流，包括主机订阅生命周期、观看端独立解码/呈现及交互。生产主机支持多订阅及逐屏视频发送，v55 增加逐屏光标。本次已接入原生独立 GPU 解码器和 Flutter 多纹理视图、输入坐标、光标与焦点截图/录制，当前进度见 [观看端对照](D:/ds-chcast/docs/multidisplay-viewer-rustdesk-2026-11-08.md)。共享连接的多个独立窗口、复杂拓扑恢复和实机验收仍未完成。

参考 `D:/rustdesk-ref` 提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`：`src/server/connection.rs::capture_displays`、`src/server.rs::capture_displays`、`src/flutter.rs::session_switch_display`。没有改动参考源码。

## 确认的选择规则

1. 非空 `add` 优先，其次非空 `sub`，最后执行 `set`。
2. `add` 把有效索引加入当前集合；重复项不产生额外订阅。
3. **`sub` 保留列表内已存在的订阅**，不创建新服务；它不是删除列表内的屏幕。
4. `set` 替换整个集合，空 `set` 退订全部视频服务。
5. 正在执行的非空列表若全部无效，保留当前订阅；若部分有效，只处理有效项。
6. 请求作用于该连接；其他连接的订阅及系统声音、文件、输入等独立服务不应随之退出。

这些规则由 `src/host/display_selection.h::select_capture_displays` 实现，并已接入 `HostServer::stream_desktop` 的订阅管理。

## 本轮实现

- 协议升至 **v54**，`FrameHeader` 从 48 字节扩为 52 字节，末尾增加 `stream_id`。0 保留给原有单桌面/摄像头路径；多个独立桌面使用连接内不复用的 ID，供观看端区分解码器及过滤已退订的视频。
- `CaptureDisplays` 携带请求 ID 和 add/sub/set 三个列表，每个列表最多 64 项。QUIC 按 FIFO 保存最多 32 个待处理操作，超限报告协议错误，不能像旧 `SwitchDisplay` 那样只保留最后一次。
- `CaptureDisplaysResult` 返回完整实际订阅集合及每个流的源尺寸、编码尺寸、原点、格式等描述。拒绝也返回仍然生效的集合；重复屏幕、重复流 ID、非法结果和截断描述会被拒绝。
- `DisplayStreamInfo` 按流更新描述，避免不同显示器互相覆盖。收到新集合后清除较旧的待读描述；已退订流的迟到更新不再进入队列。流描述不修改连接权限或音频状态。
- `DisplayKeyframeRequest` 指定流 ID，重复的未处理请求合并，不影响其他流。原有全局关键帧请求保留给单屏路径。
- 握手边界按认证已接受/HelloAck 已解析来判断就绪，避免新控制消息紧随 HelloAck 到达、应用线程尚未从阻塞调用返回时丢消息。连接重置清除所有新增队列和映射。
- 视频仍共用原有有序 QUIC 视频通道及 v52 的有限发送窗口、累计取帧确认；序号在连接内递增，流 ID 标记每个访问单元的归属。

## 生产主机接入

- `DesktopVideo` 保存每个订阅的采集器、编码适配器、流描述、IDR 状态、缓存图像、码率、FPS、缩放及积压恢复状态；原有捕获/重建/发送逻辑复用于各个屏幕。每轮改变先发送哪一路，避免发送窗口紧张时一直优先同一显示器。
- 准备新增目标后才删除旧订阅。目标采集或编码失败会销毁临时订阅，返回原集合；新加入的显示器获得连接内不复用的流 ID，已有订阅保留 ID。
- 空 set 释放全部视频订阅，连接循环继续处理系统音频、文件、终端、端口、权限与选项。再次 add/set 使用原连接，音源不重开。
- 桌面连接的 `CodecPeer` 持续到连接退出，即使它暂时不订阅画面也保留解码能力与编码偏好；新增 `vote_peer` 支持零订阅期间修改偏好。每连接仍只投一票，多个视频流不重复参与投票。
- 每路发送带流 ID 的访问单元，按源处理重建和 IDR 恢复；帧率/码率改变也在静态屏幕上发送逐屏描述。普通 `StreamInfo` 继续承载连接级权限和声音，额外视频流的描述不覆盖它。
- 多流模式中的 SwitchDisplay 会加入/聚焦目标，保留其他订阅，后续 set/sub 决定移除哪些流，与参考源码的现代多窗口分支相同。物理模式请求作用于已订阅的目标，操作完成后恢复原焦点；多屏期间自动跟随暂停切换，单屏时继续。
- v55 已补齐所有订阅屏幕的光标发布，包括静态首帧、跨屏显隐、退订清理及迟到消息过滤；光标读取共享源的最新快照，不跟随该连接积压的视频队列。观看界面的独立光标呈现已接入；虚拟屏/隐私拓扑的完整集合恢复尚待完成。

当前协议为 **v55**；主机、服务 worker、观看端需要共同升级。v54 引入的视频头仍为 52 字节，v55 新增 `DisplayCursorUpdate` 控制消息。本项目仍不与 RustDesk 的线协议互通。

## 验证范围

- `protocol`：原有消息和视频头回归，新增完整 32 位流 ID、52 字节头、旧版本拒绝。
- `protocol_fuzz`：新增四种控制消息作为有效种子，覆盖截断、长度篡改、位翻转、插入和随机尾部。
- `display_subscriptions`：选择语义、重复/无效索引、空集合、过大列表、结果 ID 去重、无输入权限的订阅请求、消息角色限制；真实 QUIC 回环核对两屏访问单元交错传输的 ID/序号/CRC、不同屏的描述更新、退订清理、握手后即时初始状态、连接重置及非桌面会话拒绝。
- 这里的网络测试直接驱动两个 `QuicSession`，不调用生产 `HostServer` 的多屏调度，也不调用真实 GPU 解码器。其结果只证明协议和传输能力。

最终日志 `build/multidisplay-v54-final-tests.log`：`protocol` 0.07 秒、`protocol_fuzz` 0.67 秒、包含 17 个场景的 `host_parity` 39.63 秒、`display_subscriptions` 0.19 秒，全部通过。`build/multidisplay-v54-hardware.log` 验证升级视频头后的实际 HEVC GPU 同屏双连接仍可解码，首位退出后资源不重建；这是原有单屏路径的回归，不是单连接多屏 GPU 验收。

`chast-host`、`chast-service`、`chast-quic`、`chast-viewer-probe` 和相关测试目标构建通过，日志 `build/multidisplay-v54-products-build.log`；保留已有的 LIBCMT 链接及 `normalized` 变量遮蔽警告。没有部署、重启服务或改变物理显示布局。

生产主机接入后的验证（以本段为当前结果）：

- `build/multidisplay-v54-host-final-tests.log`：包含 **20 个场景**的 `host_parity` 通过（46.26 秒）；`desktop_stream` 通过（2.75 秒）；`display_subscriptions` 通过（0.19 秒）。主机新增单连接两屏静态首帧、逐屏 FPS 公告、sub 保留、退订/重加 ID、单屏重建、零订阅期间声音及文件继续、失败回退、每连接一票，以及停读后操作与两路恢复。
- 产品及主机测试构建通过：`build/multidisplay-v54-host-final-build.log`。新增 GPU 订阅退订/重加检查的测试目标构建通过：`build/multidisplay-v54-hardware-build.log`。
- **本轮硬件验收没有通过**：`--hardware-auto` 和 `quic_stream_options` 均在首次采集处得到 `DuplicateOutput: 0x80070005`，未到达新增订阅断言；日志 `build/multidisplay-v54-host-hardware.log`、`build/multidisplay-v54-host-hardware-retry.log` 和上述最终 CTest 日志。当前桌面检查显示 Default 且未锁定；运行 2026-10-27 已构建、未包含本轮主机变化的 `chast-capture.exe --seconds 1 --quiet` 也报同样错误（`build/multidisplay-v54-capture-environment.log`）。这是环境异常的证据，不是本轮硬件功能正确的证明。没有改为 CPU 回退、跳过断言、重启服务或改变物理桌面来制造通过结果。

## 必须继续完成

v55 的逐屏光标实现和最新验证见 [多屏光标对照](D:/ds-chcast/docs/multidisplay-cursors-rustdesk-2026-11-08.md)：5 组 CTest 通过，主机扩至 21 个场景；产品构建通过。硬件验收仍是上一轮的访问拒绝状态。

1. 原生独立 decoder/sink、Flutter 多纹理和逐屏输入、焦点截图/录制已接入；继续补齐共享连接的独立窗口生命周期和每窗口的控制状态，不能把网格视图等同为 RustDesk 多窗口。
2. v55 的逐屏光标已接入原生和 Flutter，按源尺寸呈现，并独立于状态刷新每 33 ms 取更新。主机采样仍依赖 DXGI，尚未复刻 RustDesk 独立光标服务的节奏。
3. 显示器消失、重新编号、几何变化和适配器替换的多流恢复已实现，单屏故障不再阻塞健康源协商，见 [拓扑与恢复对照](D:/ds-chcast/docs/multidisplay-topology-rustdesk-2026-11-08.md)。继续完成虚拟屏/隐私拓扑切换后的原始完整多选集合恢复；自动回归不能代替物理拓扑实机验收。
4. 采集环境恢复后执行已添加的 GPU 订阅/退订/重加检查和参数切换回归，并使用真正双显示器/不同适配器验证多解码器与驱动故障；本机同屏双连接不代替该验收。

整体 RustDesk 功能复刻目标保持进行中；其他尚未复刻模块仍按主机模块对照报告推进。
