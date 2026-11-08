# 独立选屏：观看端接入与 RustDesk 对照

参考本地 `D:/rustdesk-ref`，提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`。本次延续 v55 主机订阅实现，补齐同一连接内多显示流的原生接收及 Flutter 独立画面。协议版本保持 v55。

## 已实现的行为

| 项目 | RustDesk 参考 | 当前实现 |
| --- | --- | --- |
| 连接的选屏集合 | `src/server/connection.rs::capture_displays`；add 优先于 sub，sub 保留列出的订阅 | 保持相同语义；界面发送完整 set，逐屏关闭只更新本连接的集合 |
| 独立解码 | `src/client/io_loop.rs` 按 `vf.display` 路由到 `video_threads` | `DisplayVideoReceiver` 每路一个工作线程、GPU 解码器、输出及有界队列；某路阻塞不阻塞另一屏 |
| 逐屏恢复 | 每屏解码状态及刷新 | 初次绑定、尺寸变化、损坏数据、队列超限均逐屏等待 IDR，拒绝旧尺寸和已退订来源 |
| 逐屏显示 | 每显示器 renderer | 菜单新增“全部显示器（独立画面）”；每路一个 Flutter GPU 纹理，可选焦点、单独关闭及重加 |
| 输入坐标 | 视图按源显示器原点映射 | 每个视图使用自身源尺寸及原点；按键点击、滚轮、手势前先把鼠标移动到该屏坐标，触摸/笔也使用该屏几何信息 |
| 光标 | `CursorModel` 按视图原点换算 | 逐屏 BGRA 精灵按源尺寸缩放与裁剪；位置更新保留已有同形状精灵；未知新形状不显示旧图；只读视图也显示光标 |
| 零订阅 | 视频服务可全部退订，连接仍存在 | 关闭最后一屏保留工具栏和服务；可以在原连接重新添加屏幕，不触发旧单屏 20 秒无帧超时 |
| 拓扑与故障恢复 | 显示列表每 300 ms 检查，各显示器有独立视频服务 | 多流模式每 300 ms 对账；移除消失源、保留存活流；单屏采集或编码失败逐屏重试，其他屏继续；详见下方拓扑报告 |
| 生命周期 | UI 对应的 renderer 独立释放 | 退订先同步清除 native output 指针，再 unregister 纹理；重连等待未完成的注册/绑定结束，旧控制器不能绑定到新会话 |

原生累计 decoded/published 统计保留已退订来源的数量；`ready` 单独表示当前绑定和格式已有画面，避免使用累计帧数误判新纹理已经就绪。总 FPS 包含各显示流的发布帧数，**不是单屏 FPS**。

截图捕获点击时的焦点来源，在该屏工作线程串行读回；之后切换焦点不会改变已经排队的截图目标。输出解绑/重建会使旧读回任务失败，避免从新屏或释放后的输出读取。录制只记录开始时选定的一个流，焦点变化不混入另一屏；退订录制源会结束并封口文件。开始录制只请求目标流的 IDR。该策略尚不等于 RustDesk 多个 UI 会话分别控制录制。

入口：`app/windows/viewer/display_video_receiver.{h,cc}`、`viewer_engine.{h,cc}`、`viewer_bridge.{h,cc}`，以及 `app/lib/display_stream_bridge.dart`、`display_stream_controller.dart`、`widgets/display_stream_view.dart`、`pages/session_page.dart`。

## 验证与构建

新增测试使用假 GPU 边界来控制解码阻塞、失败及线程销毁，传输仍使用真正的回环 QUIC 和证书固定校验。Flutter 测试模拟插件通道，检查真实 SessionPage 的纹理、选屏和输入调用。它们不证明实际多 GPU 解码及纹理显示已经通过。

- `display_video`：独立工作线程、有界队列、CRC、尺寸变化、退订/重加、CPU 帧拒绝、读回与渲染串行、解绑取消截图、退订累计计数。
- `viewer_display_streams`：无输入权限的原生观看器多屏路由、光标、焦点截图、空订阅保持 21 秒、重加与关闭；新增 MP4 交错流录制验证。
- Flutter：控制器的保留纹理、退订释放顺序、注册途中关闭、失败后安全释放；SessionPage 多屏、负原点点击、空集合/重加；光标源尺寸缩放、裁剪、形状缓存和输入穿透；菜单、旧坐标及启动轮询回归。

最终运行结果在本文件末尾记录。此前 `build/multidisplay-viewer-final-tests.log` 的第一次运行发现当前 `D:/vcpkg/installed/x64-windows-static/lib/avformat.lib` 没有 MP4 muxer。该依赖裁剪问题会使旧单屏及新多屏录制都失败。测试没有跳过此断言。

工作区辅助构建脚本 `tools/build_recording_ffmpeg.ps1` 验证源代码和已安装 FFmpeg 的公共版本一致，仅重建支持 MP4/MOV 的容器库，并把原有硬件 codec 库复制到独立前缀。它不修改全局 vcpkg 安装。使用其输出作为 `-DFFMPEG_ROOT=.../prefix`，再构建产品与录制测试。生产采集/编解码仍是 GPU 路径。

当前构建命令（已有目录保留其他 CMake 配置）：

```powershell
./tools/build_recording_ffmpeg.ps1 `
  -SourceRoot D:/vcpkg/buildtrees/ffmpeg/src/n8.1.1-98a88eee95.clean `
  -ExistingPrefix D:/vcpkg/installed/x64-windows-static `
  -MsysRoot D:/vcpkg/downloads/tools/msys2/601511c4f96fbeee `
  -VisualStudioRoot 'C:/Program Files/Microsoft Visual Studio/18/Community'
cmake -S . -B build -DFFMPEG_ROOT=D:/ds-chcast/build/ffmpeg-recording/prefix
cmake -S app/windows -B build/flutter-multidisplay-check -DFFMPEG_ROOT=D:/ds-chcast/build/ffmpeg-recording/prefix
```

上述源码/MSYS/VS 路径为本机实际安装；其他环境传入对应路径。此前旧 `app/build/windows/x64` CMake 缓存缺少 C++ feature 信息，所以使用独立的 `build/flutter-multidisplay-check` 验证，没有删除原缓存或降低编译标准。

最终原生结果：`build/multidisplay-viewer-mp4-tests.log` 中 **6 组全部通过**：`display_video` 0.27 秒、`viewer_display_streams` 23.64 秒、`clipboard_async` 16.87 秒、`mp4_recorder` 0.24 秒、`viewer_files_only` 3.77 秒、`viewer_files_only_denied` 0.76 秒。实际录制文件重新解封装，确认只含录制目标来源的两个样本；夹在其间的另一屏帧不进入文件。相关目标构建日志为 `build/multidisplay-viewer-mp4-build.log`。

Flutter **60 项测试全部通过**，11 个相关文件的静态分析无问题；日志分别为 `build/multidisplay-ui-final-tests.log`、`build/multidisplay-ui-final-analyze.log`。最新完整查看器构建日志为 `build/multidisplay-ui-mp4-build.log`。光标只通知对应纹理的视图刷新，不以 33 ms 频率重建整个会话工具栏。构建仍有第三方 FFmpeg 的转换及已有 LIBCMT/LNK4199 警告。原有 avcodec、avutil、swresample、swscale 静态库与新前缀的文件哈希逐一相同。

## 仍未完成的对齐

物理拓扑与故障隔离的后续实现、回归结果和身份策略差异见 [拓扑与恢复对照](D:/ds-chcast/docs/multidisplay-topology-rustdesk-2026-11-08.md)。原生查看器已修复无请求 ID 的拓扑通知覆盖显式选屏应答的问题，避免 Flutter 一直保留选屏忙碌状态。

1. 现有“在新窗口打开显示器”仍创建另一个连接；本次多屏网格共享一条连接，但没有复刻 RustDesk 多窗口复用同一会话和订阅并集的完整生命周期。
2. “完整桌面”现在由观看端按远端坐标拼接逐屏纹理；单屏、独立网格、完整桌面和服务端默认可在同一连接内切换。逐屏订阅期间界面暂不开放物理分辨率修改和相对鼠标锁定。
3. 已覆盖物理屏消失、重编号、位置/尺寸/旋转变化和显卡身份替换的生产主机恢复逻辑；测试使用模拟采集边界，物理多屏实机验收仍未通过。虚拟屏和隐私屏切换后恢复原始完整多选集合仍需补齐。
4. Flutter 已将取光标更新独立为 33 ms 定时任务，不等待 200 ms 状态轮询；主机采样仍来自 DXGI，尚未复刻 RustDesk 独立光标服务的采样节奏。
5. 最近硬件采集验收在 `DuplicateOutput: 0x80070005` 处失败，详情见独立流报告。本次没有证据说明采集环境已恢复，也没有把模拟解码测试描述成多屏 GPU 实机通过。

整体复刻目标继续进行，以上接口/自动测试通过不能据此宣称与 RustDesk 全部功能一致。
