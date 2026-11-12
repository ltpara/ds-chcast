# OpenH264 软件编码兜底（已废弃）

> **历史文档（2026-11-12 晚起失效）**：产品软件回退已改为 VP9（libvpx），OpenH264 编码器、CMake 依赖与 `OpenH264::openh264` 链接均已从仓库移除。`EncoderBackend::Software` 只接受 VP9，Auto 在硬件全失败后协商软件 VP9。本文保留 2026-11-08 至 2026-11-12 的实现与验证记录，仅作追溯；现行行为见 [sender-effectiveness-rustdesk-2026-11-11.md](sender-effectiveness-rustdesk-2026-11-11.md) 第 16、17 节和 [encoder-quality-comparison-2026-11-11.md](encoder-quality-comparison-2026-11-11.md)。

2026-11-08，在原有 oneVPL、FFmpeg 厂商编码后增加 OpenH264。

## 行为

- `EncoderBackend::Auto` 依次尝试 oneVPL、FFmpeg 厂商硬件编码、OpenH264。只有前两者创建失败才进入软件路径。显式 `Vpl`/`Ffmpeg` 保持固定后端，`Software` 直接使用 OpenH264。
- OpenH264 输出 H.264 Annex-B；HEVC/AV1 请求落到软件路径后，`codec()` 返回 H.264。桌面共享服务仍验证订阅者能力交集及候选首帧，不把 H.264 码流标成 HEVC/AV1。
- 使用 D3D11 staging 读回 NV12，并按映射的 RowPitch 转为 I420。重复帧使用缓存的 I420；支持强制 IDR、PTS 和无重建的码率/FPS 更新。
- 使用屏幕实时模式、时间戳码率控制、允许拥塞跳帧、最多四个编码线程。每个访问单元加 AUD，避免多切片被离线重放误分为多帧。
- 周期 GOP 为零时不安排周期 IDR，但 OpenH264 屏幕模式会自行检测场景变化并生成 IDR；纯码率变更本身不要求 IDR。
- FPS 参数范围 1–60；这表示配置范围，不保证 CPU 实际吞吐。软件路径仍依赖有效的 D3D11 NV12 输入，并不等于完全移除显卡/图形设备依赖。
- 光标与码率查询读取视频服务发布的状态快照，格式、尺寸、码率和 FPS 一起更新，避免每次查询排队等待软件编码。采集、编码和重配操作仍在所属视频线程执行。

## 运行中恢复

- 共享桌面服务遇到普通编码错误先以 100 毫秒间隔重试，静止桌面也会使用保留的 NV12 输入重试。连续三次失败后，原始配置为 Auto 且实际后端为硬件时，禁用该显示源的硬件路径，发起软件 H.264 协商。`IVideoEncoder::backend()` 显式报告实际后端，不依赖实现名称判断。
- 只对故障显示源强制 Software。全体桌面连接仍按解码能力交集和偏好协商，所有候选先验证 IDR，再一起提交。健康显示源可继续用硬件 H.264；不会把软件 H.264 标成 HEVC/AV1。新代次禁止旧适配器重放故障前的缓存帧，保留有效采集设备及纹理。
- 若 HEVC-only 等接收端阻止 H.264 协商，故障会话保留连接、音频和控制，视频每秒重试；限制连接离开后自动继续。协商失败不会替换其他健康显示源的编码器。硬件排除状态在该显示源存续期间保留，包括采集重建和新偏好，避免反复选回故障后端；显示源完全释放后重新创建时才重新尝试硬件。
- 固定 Vpl/Ffmpeg 不会悄悄转软件；软件编码器持续失败也会明确报错。DeviceRemoved/DeviceReset 走采集设备重建，不在失效设备上尝试软件读回。重复编码失败后若直接编码可成功，会要求 IDR，避免继续依赖可能损坏的参考帧。

## 依赖和构建

```powershell
D:\vcpkg\vcpkg.exe install openh264:x64-windows-static-md
cmake -S . -B build -DOPENH264_ROOT=D:/vcpkg/installed/x64-windows-static-md
cmake --build build --config Release --target chast-host chast-service chast-encode chast-decode chast-quic -j 6
```

本机安装的是 OpenH264 2.6.0、vcpkg port revision 4，直连下载成功。静态库使用动态 CRT，与应用 `/MD` 一致；保留项目原有 FFmpeg/Opus 的 LIBCMT 链接警告，未用屏蔽警告掩盖它。CMake 同时指定 Debug、Release 与 Flutter Profile 映射。OpenH264 的第三方声明已加入根目录 `THIRD-PARTY-NOTICES.md` 并由 Windows 打包脚本复制。

## 验证

- 编码器及工具构建记录：`build/openh264-final-build.log`；状态快照修复后的主机和 QUIC 工具构建记录：`build/openh264-release-build.log`；服务程序构建记录：`build/openh264-service-build.log`。均构建成功，未安装或重启系统服务。
- `build/openh264-check-final.log`：一次性 WARP 验证程序使用真实编码库与 FFmpeg 解码器；Auto 在无厂商硬件编码器的设备上选择 OpenH264，AV1 请求降级 H.264；固定硬件后端拒绝软件回退；重复帧的 IDR/PTS、1.5M/30 → 4M/60 → 0.5M/15、较大源纹理的裁剪与 Y/U/V 内容均通过；固定内容调码率后的帧仍为 P 帧。此程序仅位于忽略的 `build/` 下，没有增加 CMake 测试工程。
- `build/openh264-gdi-final.log`：实景 GDI 采集覆盖 HEVC 请求降级、提升码率/FPS 与显式关键帧；1920×1080 输出 9 帧、解码 9 帧、2 个关键帧、零错误，实测约 5.3 fps。
- `build/openh264-metadata-tests.log`：现有 `host_parity`（59.05 秒）和 `desktop_stream`（3.44 秒）通过。后者增加一项慢编码检查：编码线程停顿 700 毫秒时，光标及匹配格式的码率查询合计小于 100 毫秒。既有格式切换、旧格式拒绝新码率、资源线程归属等检查继续通过，没有新增测试工程。
- 运行中恢复构建：`build/runtime-encoder-final-build.log`，主机、服务、编码/QUIC 工具及两个已有回归目标成功；主机会话用例最后补充后的构建见 `build/runtime-encoder-host-build.log`。保留原有第三方库的编译与 LIBCMT 链接警告。
- `build/runtime-encoder-tests.log`：`host_parity`（64.05 秒）及 `desktop_stream`（4.43 秒）全部通过。覆盖静止桌面连续错误、旧代次禁止重放、单次错误恢复、设备移除、固定后端拒绝软件回退、跨屏兼容性与失败原子性，以及限制接收端离开后恢复。真实 QUIC 会话覆盖 H.264 内切后端、HEVC → H.264 跨屏切换、协商暂不可用时保留音频/连接；故障显示源创建一次软件编码器，健康源不切软件，两边均未因编码故障重建采集器。编解码故障由既有媒体替身注入，这部分不证明真实码流质量。
- `build/runtime-openh264-check.log`：一次性程序在忽略的 `build/` 中使用实景 GDI 采集、真实 Intel `mfx-gen (VDENC HEVC)` 和 OpenH264 2.6.0。首次硬件画面成功后，在编码接口处注入持续错误，生产共享视频服务自动协商到软件 H.264；960×540 的硬件帧与软件恢复 IDR 各解码成功一次，恢复代次一次、采集工厂调用一次，恢复后码率/FPS 更新成功。这是接口故障注入，不是实际破坏驱动或拔除显卡；没有新增 CMake 测试项目。

```powershell
.\build\Release\chast-encode.exe --encoder software --capture gdi --codec hevc --seconds 1.6 --bitrate 1500 --framerate 30 --bitrate-switch 600:3500:60 --keyframe-interval 700 --out build/openh264-gdi-smoke.h264
.\build\Release\chast-decode.exe --codec h264 --in build/openh264-gdi-smoke.h264 --quiet
.\build\Release\chast-quic.exe verify --port 45584 --seconds 12 --encoder software --codec h264 --stream-options --framerate 15
```

最终网络记录为 `build/openh264-quic-verified.log`，进程退出码 0，`QUIC loopback PASSED`。12.176 秒内 32 个访问单元全部收到并解码，6 个关键帧；CRC、序号、协议、解码错误及视频发送错误均为零。覆盖 1920×1080 → 1440×810 缩放、6 次流描述更新、动态画质/码率、不可用 HEVC 偏好的保留及继续使用 H.264，以及切屏成功与不存在的显示器拒绝。配置 15 fps 不表示本机实测达到 15 fps。

同次运行音频发送和接收均为 595 包，线上无序号缺失、CRC 或解码错误；主机发送前丢弃计数为 4 个 20 毫秒包（累计 80 毫秒），通过原有“不超过 8 包”的重配验收上限，不能称为全程零丢帧。该验证模式仍跳过文件离线重放，离线解码证据来自上面的 GDI 与 WARP 专项运行。

验证器原先把“纯码率变更期间出现 IDR”一概认作重启，现将 OpenH264 的场景检测行为与硬件后端的精确 IDR 数量断言分开；仍检查码率公告、格式/FPS、解码和音视频会话。切屏拒绝探测改为收到前次成功响应后再等待 1.2 秒，避免将服务端处理延迟误判为无效索引处理失败。

保留此前失败日志用于追溯：`openh264-quic-final.log` 有切屏冷却与音频丢弃超限，`openh264-quic-diagnostic.log` 的媒体与音频检查通过，但剪贴板实际写入两次、成功统计仅一次。最终运行剪贴板写入与统计均为两次；未因此宣称所有异步剪贴板竞态已经解决。普通短时 `verify` 对恒定 FPS 和公告次数的断言还与共享 QoS 的自适应初始限帧冲突，该模式的旧失败日志不能当成全绿验收。

创建阶段及运行中普通硬件编码错误的软件兜底已接入。4:4:4 色度、摄像头能力协商、真实双机弱网及真实驱动崩溃/不同显卡故障验收仍属于后续范围。临时 H.264 视频已清理，诊断日志保留。

## 硬件编码的最低驱动

厂商编码器是否存在由编译期决定，能否运行由驱动决定。FFmpeg 的 nvenc 在 `avcodec_open2` 时用 `NvEncodeAPIGetMaxSupportedVersion()` 读取驱动上报的 NVENC API 版本，与编译时 `ffnvcodec` 头文件的版本比较，不足即返回 `ENOSYS`，不会再尝试编帧。

本仓库 vcpkg 的 `ffnvcodec` 为 **13.0**，因此：

| NVENC API | Windows 最低驱动 |
| --- | --- |
| 11.1 | 471.41 |
| 12.0 | 522.25 |
| 12.1 | 531.61 |
| 12.2 | 551.76 |
| 13.0 | **570.0** |

2026-11-12 在 GTX 960 上实测（**早于当晚的 VP9 回退切换**）：驱动 560 系列（NVENC API 12.2）时全部 `*_nvenc` 初始化失败，`EncoderBackend::Auto` 回退 OpenH264，1080p30 合成画面帧年龄约 1.3 秒；升级到 582.78（API 13.0）后走 `h264_nvenc`，帧年龄降到 3–4 毫秒，全部 74 项原生测试通过。当下同样的驱动失败会回退软件 VP9，不再创建 OpenH264。AMD AMF 与 Intel QSV 各自有独立的运行时门槛，但失败表现相同：后端存在、驱动拒绝。

对应改动：

- `hardware_encoder_available()` 在拿到设备时改为真正打开一次厂商编码器（640×360 试编），按适配器 LUID 与编码格式缓存结果，不再只查 `avcodec_find_encoder_by_name`。这样"这台机器能不能编 H.264"的回答与实际会话一致。
- NVENC 打开失败且驱动版本低于编译期要求时，错误信息带出驱动上报版本、编译期要求版本和最低驱动号，可直接据此升级；其它失败保持原始 libavcodec 错误。
- 固定 `Vpl`/`Ffmpeg` 请求 VP9 时报错改为"该后端没有 VP9 编码器（VP9 仅软件）"，不再笼统地说"VP9 可通过软件后端获得"。
- 桌面编码协商的候选回退链保留**首个**候选的失败原因。此前最后一个候选（VP9）的错误会覆盖真正的 NVENC 驱动错误，导致日志误导。
