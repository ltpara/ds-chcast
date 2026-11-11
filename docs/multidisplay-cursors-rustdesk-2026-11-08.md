# 多屏光标：v55 实现与 RustDesk 对照

本轮补齐生产主机的逐屏光标发送及 QUIC 接收队列，修复连续位置消息覆盖未消费形状图片的问题。当前只证明主机和传输行为；原生观看器的多解码器、Flutter 多纹理及逐屏光标呈现尚未完成。

参考仍是本地 `D:/rustdesk-ref` 提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`。参考入口：`src/server/input_service.rs::new_cursor/new_pos/run_pos`、`flutter/lib/models/model.dart::CursorModel.updateCursorPosition/updateDisplayOrigin`。RustDesk 使用独立的光标形状、位置服务及全局桌面坐标，视图按显示原点映射；本项目使用每路视频的源坐标和 `stream_id`，对齐可观察行为，不使用相同线协议。

| 行为 | 本轮实现及验证 |
| --- | --- |
| 每位观看者按自己所选屏幕接收光标 | 每连接、每路 `DesktopVideo` 保留独立发送状态；同屏复用不可变光标快照 |
| 同一连接的多路光标 | 新增 `DisplayCursorUpdate`，携带 32 位流 ID 和原有位置、显隐、形状内容；额外屏幕不再跳过光标发布 |
| 首次进入多流模式 | 即使原单屏已经发送过静态形状，也重新发布完整逐屏快照 |
| 跨屏移动 | 原屏隐藏、新屏显示，并保留各自源坐标；不要求键鼠权限 |
| 观看者停止读取视频 | 在检查视频发送窗口之前发布光标，读取共享源最新状态，不使用该观看者队列中的旧画面光标 |
| 连续位置消息合并 | 形状 ID、尺寸相同时保留未消费的图片；不同形状、尺寸不能沿用旧图片；已消费的图片不重复附加 |
| 退订、空集合及重新加入 | 丢弃已退订流的待消费/迟到光标，保留存活流的未消费图片；重加使用新 ID 并发送完整形状 |
| 另一观看者仍在看同屏 | 退订及空集合只影响当前连接，另一连接继续获得该屏光标 |

## 实现入口

- `src/host/desktop_stream.{h,cc}`：`cursor_snapshot` 返回拥有生命周期的只读快照，和订阅者的视频积压独立。重建清除当前快照，已经取得的旧快照仍安全；它不持有 GPU 图像。
- `src/host/host_server.cc`：逐屏发布、进入多流模式时重发静态形状、发送窗口关闭时仍发布最新光标。物理模式切换也将借用的 `FrameInfo` 改为值副本，避免编码协商期间失效。
- `include/chast/protocol.h`、`src/net/protocol.cc`：协议 **v55**，控制类型 74；视频头仍是 v54 的 52 字节。新消息复用原有光标格式校验，拒绝截断、尾部数据及无身份的图片。
- `src/net/quic.cc`：以已公告订阅集合限制逐屏队列，按流合并；重置会话清空。旧单屏光标队列同步修复形状丢失。
- `app/windows/viewer/viewer_engine.cc`：旧单屏 UI 待消费队列也保留尚未送给界面的形状图片。

## 验证

`build/multidisplay-v55-cursor-wire-tests.log`：

- `protocol` 通过，0.07 秒：新增完整 ID、负坐标、热点/形状往返、所有截断位置和合并语义。
- `protocol_fuzz` 通过，0.65 秒：新增带图片的逐屏光标种子，并增强旧光标种子。
- `display_subscriptions` 通过，0.34 秒：真实 QUIC 验证逐屏合并、单屏兼容、退订迟到消息、存活流图片保留、空集合清理、重加及角色限制。

`build/multidisplay-v55-cursor-host-tests.log`：

- `host_parity` **21 个场景全部通过**，49.86 秒：新增模拟双屏、真实 QUIC 的主机光标场景，覆盖只读连接、静态初始快照、跨屏移动、两位观看者、视频停读、sub 保留及重加。
- `desktop_stream` 通过，2.76 秒：增加最新光标与视频队列隔离、旧快照不可变、重建清空及快照不阻止 GPU 资源释放的检查。

构建通过：主机、服务、QUIC 工具、原生查看器探针及上述测试目标，日志 `build/multidisplay-v55-cursor-build.log`、`build/multidisplay-v55-cursor-final-build.log`。保留已有 LIBCMT 冲突和测试目标无 d3dcompiler 延迟导入警告；`git diff --check` 通过。

## 未完成的对齐

1. 原生多解码器和 Flutter 多纹理已接入逐屏光标，并验证缩放、裁剪和形状缓存，见 [观看端对照](D:/ds-chcast/docs/multidisplay-viewer-rustdesk-2026-11-08.md)。共享连接的多个独立窗口仍未完成。
2. ~~光标采样仍依赖 DXGI 采集节奏~~（2026-11-11 补记：显示源已有约 33 ms 独立指针线程，单屏和合并流都接入，见 `status-and-plan-2026-11-09.md` 的 v71 条目）。RustDesk 独立光标服务以约 33 ms 轮询。
3. 隐私屏、虚拟屏和物理显示热插拔的完整多订阅恢复仍需实现和验证。
4. 真实 GPU 验收仍沿用上一轮的未通过状态：`DuplicateOutput: 0x80070005`。本轮没有环境变化证据，因此没有重复运行同一失败的硬件检查；模拟媒体测试不能替代真实双屏/跨适配器验收。

主机、服务 worker、查看器须共同升级到 v55。本轮未部署、重启服务、修改显示布局或注入真实输入。
