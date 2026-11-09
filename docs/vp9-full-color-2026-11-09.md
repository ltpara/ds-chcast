# VP9 / 4:4:4 实现进度与 v61 协议（2026-11-09）

本文是当日较早检查点的历史记录：当时主要实现已接入，整体会话验收尚未完成。后续 P0-3/P0-4、1080p 性能及打包收尾记录以 [最新状态入口](status-and-plan-2026-11-09.md) 和 [开发检查点](development-checkpoint-2026-11-09.md) 为准；本文的“下一步”和环境边界描述保留为当时状态，不代表最新完成度。

## 1. 当前实现

- `src/codec/vpx_encoder.cc` 使用 libvpx VP9 realtime、CBR、`lag_in_frames=0`、关闭 alt-ref，最多 8 个线程。profile 0 使用 NV12 转 I420；profile 1 读取原始 BGRA，必要时先缩放 BGRA，再转 I444。支持缓存重复帧、强制关键帧及动态码率/FPS。
- `src/codec/vpx_decoder.cc` 与 `vpx_internal.h` 校验 I420/I444，CPU 解码后转 BGRA 上传 GPU；完整色彩不会先降为 NV12 再恢复 BGRA。VP9 是软件路径，不等于整条链路不再依赖 D3D11。
- 编码器工厂只在 Auto/Software 下接受 VP9；固定 Vpl/Ffmpeg 不隐式转软件。当前 H.264、HEVC、AV1 只接受 4:2:0；OpenH264 仍是软件 H.264 兜底。
- `DesktopStreamPool` 保留原始 BGRA 与 NV12，按编码器输入要求选用纹理，传播实际 codec/chroma，并按变化重建流和解码器。
- ViewerEngine、逐屏 DisplayVideoReceiver 和 GpuFrameSink 支持实际色度变更与 BGRA 直接复制；多屏主状态从当前焦点流更新。
- Flutter 提供 VP9 和标准/完整色彩选项并保存偏好；开启完整色彩时选择 VP9，切换其他 codec 时清除该偏好。偏好与最终实际格式分别处理。
- MP4 录制支持 VP9 profile 0/1 和实际色度，格式切换分段，允许 VP9 无 extradata。当前无 alt-ref / 无延迟的单帧输出允许绕开精简 FFmpeg 缺失的 `vp9_superframe` 自动 BSF；更改输出结构后必须重新检查该假设。

## 2. 共享协商规则

1. 所有参与共享协商的连接按实际 decoder mask 取交集；桌面附带摄像头沿用该连接的一票。连接暂时零显示订阅时仍保留能力和偏好。
2. 在交集中统计有效 codec 偏好，多数优先；平票按较早连接稳定选择。没有有效偏好时，当前顺序为 HEVC → H.264 → AV1 → VP9。
3. 只有全体连接都偏好 4:4:4，且都声明支持所选 codec 的 4:4:4 解码，才尝试完整色彩。目前只有 VP9 可以满足该条件。
4. 对全部活动显示/摄像头源先 prepare 编码器和候选关键帧，全部成功后一起 commit。4:4:4 失败先对同 codec 重试 4:2:0，再尝试其他共同 codec；全部失败保留原状态。
5. 当前摄像头源没有原始 BGRA，不能提供真实 I444，协商会回退 4:2:0。UI 必须显示实际格式，而不是只显示用户偏好。真实混合桌面/摄像头的最终效果仍需验收。

## 3. v61 线协议

本节字段以 `include/chast/encoder.h`、`include/chast/protocol.h` 和 `src/net/protocol.cc` 为准。媒体和控制协议要求版本匹配；主机、服务 worker、观看端和使用该协议的原生工具需要同步升级，不承诺 v60 兼容。

| 项 | v61 规则 |
| --- | --- |
| 协议版本 | `kProtocolVersion = 61`。 |
| codec | C++ `VideoCodec::Vp9` 枚举值为 3；线协议 codec 为 **4**。不要将枚举 ordinal 直接作为 wire codec。 |
| 能力位 | VP9 为 `0x08`，全部视频格式掩码为 `0x0f`；原 H.264 / HEVC / AV1 位不变。 |
| 色度 | `VideoChroma::Yuv420 = 0`、`Yuv444 = 1`；实际 `codec + chroma` 组合中只有 VP9 可使用 1。 |
| codec 偏好 | `StreamCodecPreference::Vp9 = 4`，Auto 仍为 0；偏好不等于最终实际 codec。 |
| FrameHeader | 总长保持 **52 字节**；从 0 开始的 offset **47** 原保留字节写入 chroma，offset 48–51 仍为 stream_id。 |
| ServerInfo / HelloAck / StreamInfo | 既有 flags 的 `0x8000` 表示实际 YUV444，未置位为 YUV420；与实际 codec 一起校验。 |
| ClientHello | 既有尾部再附加 `decoding_444: u8` 和 `prefer_chroma: u8`。前者须为普通解码能力的子集，当前仅允许 VP9 位；后者仅允许 0 或 1。 |
| SetStreamOptions | payload 从 4 字节扩为 **5**：codec 偏好 u8、max_fps u16、quality u8、chroma 偏好 u8。 |
| StreamOptionsResult | 固定部分从 7 字节扩为 **8**：result code u8、实际 wire codec u8、fps u16、quality u8、实际 chroma u8、reason 长度 u16，随后 reason 字节。 |

多字节整数沿用当前协议的小端序。收发两端检查合法格式和能力；帧头与当前流描述的 codec、色度、尺寸不一致时，必须通过既有流更新与代次处理，不把旧帧交给新格式解码器。

此前阶段的协议变更仍包含在 v61 中：v56 目录分页、v57 剪贴板方向、v58 空目录、v59 虚拟文件按需内容、v60 提权请求/状态；详细字段分别见各模块文档，不用这份增量说明代替完整解析器定义。

## 4. 依赖与本机构建入口

| 依赖 | 本机安装版本 | 用途 |
| --- | --- | --- |
| OpenH264 | 2.6.0#4 | 软件 H.264 兜底 |
| libvpx | 1.16.0 | VP9 软件编解码 |
| libyuv | 1916；source `d98915a654d3564e4802a0004add46221c4e4348` | BGRA / I444 转换及缩放 |
| libjpeg-turbo | 3.1.3 | libyuv 构建依赖 |

这些包已在 `D:/vcpkg/installed/x64-windows-static-md` 安装，使用静态库、动态 CRT。CMake 的 `OPENH264_ROOT` / `VPX_ROOT` 指向该前缀，并链接 `unofficial::libvpx::libvpx`、`yuv`。第三方声明见根目录 `THIRD-PARTY-NOTICES.md`。完整依赖表和 vcpkg 提交参照见 [开发检查点](development-checkpoint-2026-11-09.md) 第 6 节。

仅新环境缺少这些包时执行安装；此命令本身不锁定上述版本，严格复现还需要固定 vcpkg registry/baseline，这是后续打包工作的一部分：

```powershell
D:/vcpkg/vcpkg.exe install openh264:x64-windows-static-md libvpx:x64-windows-static-md libyuv:x64-windows-static-md
```

本机现有 CMake 缓存还包含 oneVPL、MsQuic、Opus、FFmpeg 等依赖。以下是已有环境的增量构建入口，不是全新机器的完整安装脚本：

```powershell
cmake -S . -B build -DOPENH264_ROOT=D:/vcpkg/installed/x64-windows-static-md -DVPX_ROOT=D:/vcpkg/installed/x64-windows-static-md -DFFMPEG_ROOT=D:/ds-chcast/build/ffmpeg-recording/prefix
cmake --build build --config Release --target chast-host chast-service chast-protocol-test chast-protocol-fuzz-test chast-host-settings-test chast-host-parity-test chast-desktop-stream-test chast-display-video-test chast-mp4-recorder-test --parallel 4
```

Flutter SDK 位于 `D:/flutter`。本机 Flutter 构建缓存使用 `FFMPEG_ROOT=D:/vcpkg/installed/x64-windows-static`，与根构建的录屏专用 FFmpeg 不同。保留现有配置时可执行：

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --build app/build/windows/x64 --config Release --target chast_viewer --parallel 4
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --install app/build/windows/x64 --config Release
```

MSVC 构建的 libyuv 提示没有其 SIMD 加速，不据此预估实测帧率。1080p 性能数据出来后再决定是否改用 clang-cl 或其他优化配置。

## 5. 验证证据与已知缺口

- 原生集成构建、Flutter Release 和 bundle 均成功，日志及本次回归总结果见 [检查点验证记录](development-checkpoint-2026-11-09.md#3-本检查点的验证记录)。
- `build/vpx-live-acceptance3.log`：真实库对 320×180 红蓝条纹的两种格式各编码/解码 8 帧；重复帧、码率/FPS 更新、强制关键帧和 GPU 上传全部通过。平均 RGB 误差 88.957（420）及 1.497（444）；只说明该样本色度保留效果。
- 同专项重新解码 profile 0/1 MP4 的全部样本并核验实际 `vpcC` 字节。本机 FFmpeg mov demux 没有完整暴露其中 profile/chroma，所以不能只根据 demux 输出的默认字段判定编码错误。
- 2026-11-09 恢复环境后原生回归 **8/8 通过**：`host_settings` 的旧断言已改为 `vp10` 未知样本，并新增 VP9 解析、写名和保存/读取往返；`host_parity` 本机 148.13 秒，CTest 超时上限已从 120 秒调到 240 秒。保存时的 7/8 历史记录见检查点记录。
- 44 项 Flutter 测试及静态分析是此前阶段结果；最后的菜单/状态修改后尚未完整重跑，不冒充最终全绿。
- 2026-11-09 P0-2 已用最终库重链本机临时程序 `build/vpx_acceptance.cc`：真实 DXGI 图案窗 → VP9 编码 → QUIC 回环 → 两个真实 `ViewerEngine` 的 GPU 回读。444 的 MAE 为 1.664/1.844/2.161，420 为 77.668/77.647/77.646；第二观看端加入/退出及 444→420→444 均收敛，主机统计 sessions≥2、codec VP9、最终 chroma 444。两个真实 `chast_viewer.exe` 窗口也完成加入/退出与 1→0→1 色度收敛；窗口缩放下 1 像素色度不可判别，像素级结论来自 GPU 回读。详见检查点记录。
- 尚待覆盖真实多人能力变化、多源失败回滚、摄像头导致的色度回退、主机/观看端实际 VP9 录屏、缩放/旋转/HDR/多屏输入和 1080p 性能。
- 原始流 CLI 的 VP9 离线回放尚未完整支持；不能沿用 H.264/HEVC Annex-B 拆包假设。软件 AV1 与 VP8 未实现。

当时的下一步为 P0-3、多源失败/摄像头回退/录屏复验及 1080p 性能与打包收尾。后续完成记录见 [最新状态入口](status-and-plan-2026-11-09.md)；真实设备与跨网待验项仍以该入口列出的边界为准。
