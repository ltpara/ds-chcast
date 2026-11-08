# 多人独立选屏：拓扑与故障恢复

参考 `D:/rustdesk-ref`，提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`，工作树干净。当前协议仍为 v55，生产采集与编解码保持 GPU 路径。本文补充 [观看端对照](D:/ds-chcast/docs/multidisplay-viewer-rustdesk-2026-11-08.md)。

## 行为对照

| 行为 | RustDesk 参考 | 当前实现及边界 |
| --- | --- | --- |
| 独立选屏 | `src/server/connection.rs::capture_displays` 按连接维护 add/sub/set，优先级为 add > sub > set；sub 保留列出的订阅 | 同样处理；空 set 只关闭该连接的视频，完全无效的非空列表保持原集合 |
| 拓扑更新 | `src/server/display_service.rs::run` 每 300 ms 检查并广播显示列表 | 多流连接每 300 ms 对账，零订阅仍接收菜单更新；显式空 set 不因热插拔自动重新订阅 |
| 屏幕消失 | 显示服务与客户端同步列表；参考 `check_display_changed` 按索引比较，注释允许拔屏后手动重连 | 释放消失源，保留其他订阅；全部消失时保留连接和声音，显示器返回后恢复主屏；不自动恢复原始所有已移除屏幕 |
| 显示器身份 | 参考视频服务及拓扑检查主要按索引组织 | 按设备名和适配器 LUID 识别存活屏幕；重编号保留流 ID，换适配器后重新订阅获得新 ID。这是明确的稳健性差异，不能称为逐项相同 |
| 位置、尺寸、旋转变化 | 每屏视频服务检查显示变化并重新配置 | 同一共享源只重建一次，各订阅同步新代次；刷新原点和尺寸，清除旧图像与光标状态，重新请求 IDR |
| 单屏暂时失效 | 每屏视频服务各自运行和恢复 | 采集/编码失败设置该流的重试时间，保持其他流和连接服务；恢复过程中仍能处理选屏与新观看者接入 |
| 编码能力 | `libs/scrap/src/common/codec.rs::update` 按连接 ID 保存解码能力 | 故障中的显示源不阻塞健康源的协商；该连接的解码能力仍参与全局交集。新目标必须验证 GPU 编码 IDR，恢复源稍后按当前协商结果验证 |

主机改动位于 `src/host/host_server.cc`、`desktop_stream.cc`、`display_topology.h`。`find_display` 找不到目标时返回集合末尾，不再把消失源解释为索引零。共享源在重新初始化后再次核对设备名和 LUID，避免初始化期间显卡变化导致旧流身份被错误沿用。

查看器的 `CaptureDisplaysResult(request_id=0)` 是主动拓扑通知。它仍更新流集合和通知计数，但不覆盖最近显式请求的 ID/错误状态，保证 Flutter 可以确认先前请求已完成。完整订阅结果替代更早的增量元数据；测试同时核对两种通知形式和实际新尺寸关键帧。

## 验证证据

新增集成场景执行生产 HostServer、多用户连接和真实回环 QUIC；只替换采集与编码的 GPU 边界，没有改变本机物理显示器或注入输入。

- `multiple_display_topology`：重编号不重建；两用户共享源仅重建一次；负原点；尺寸/旋转后的元数据与视频帧；移除源不误映射到零；全部消失/返回；空集合保持及菜单刷新；同名屏换显卡后新建流身份。
- `multiple_display_recovery_isolation`：采集重建失败期间继续选屏；健康屏接入 H264-only 用户并从 HEVC 切换至 H264；编码器重建失败仍保持健康屏和控制操作；故障屏恢复后按 H264 出图；限制用户退出后两屏恢复 HEVC。
- `recovery_preserves_display_identity`：普通重建和拓扑重建过程中适配器改变时，拒绝沿用旧流身份。
- `viewer_display_streams`：真实 QUIC 检查主动拓扑通知不会抹掉显式选择应答；原有逐屏解码、截图、录制与零订阅回归继续执行。

修复前的重现日志 `build/multidisplay-recovery-before-fix.log` 记录恢复中的源导致全局编码协商失败，随后选屏操作无法取得应答；修复后日志 `build/multidisplay-recovery-after-fix.log` 通过。

阶段回归 `build/multidisplay-topology-regression-tests.log` 为 9/9 组通过：protocol、protocol_fuzz、host_multi、host_parity、desktop_stream、display_subscriptions、display_video、viewer_display_streams、video_qos。host_multi 含硬件依赖子场景的跳过路径，该组通过不作为实际多屏 GPU 验收证据。

加入身份检查后的一次回归暴露测试只接受增量尺寸通知的时序依赖（`build/multidisplay-topology-identity-tests.log`）；已改为检查完整/增量描述和对应新尺寸关键帧。最终 `build/multidisplay-topology-final-tests.log`：包含 23 个场景的 host_parity 通过（64.27 秒），desktop_stream 通过（2.75 秒），共 2/2 通过。最新测试目标构建见 `build/multidisplay-topology-final-build.log`。

产品主机构建见 `build/multidisplay-topology-identity-build.log`；完整 Flutter Windows 查看器构建成功，见 `build/multidisplay-topology-ui-build.log`。保留已有 FFmpeg 转换和 LIBCMT/LNK4199 警告。此轮未修改 Dart；此前 60 项 Flutter 测试与 11 文件静态分析的证据见观看端报告。

## 尚未完成

共享连接的多个原生窗口、从独立画面切回合并/默认时避免重连、虚拟/隐私拓扑恢复原始完整多选集合、主机独立光标采样和实际多显示器/多 GPU 验收仍未完成。最近硬件验收在 `DuplicateOutput: 0x80070005` 失败；模拟 GPU 边界的回归不能证明这个问题已消失。整体 RustDesk 功能复刻目标继续进行。
