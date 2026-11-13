# 接收端功能对比与整改记录（2026-11-13）

## 1. 结论

本轮以 Windows 被控端的实际采集、编码、传输契约和 Flutter 接收端行为做对照，确认并修复了 4 项主要接收端缺口，以及多显示器合并捕获路径中的 2 项回归。当前源码侧的功能已有对应实现、用户可见反馈和定向自动化覆盖；`main` 上可继续开展真实硬件验收。

尚未完成的部分是：

- 真实旋转显示器上的 GPU 路径、软件兜底路径和运行中能力失效切换，必须用实际硬件验收。
- ACL D01–D03 的票据、策略物化、传输凭证、提交/续租/撤销和回环会话已在源码落地并有自动化覆盖；2026-11-13 已完成真实 `chast-host.exe` 单机回环验收，详见 [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md)。PostgreSQL 13/14 真库集成与迁移已通过；生产服务端、生产 PostgreSQL 上的 ACL 整链、真实反向代理与双机 Flutter 查看器仍未验收。

## 2. 功能缺口与当前状态

| 编号 | 对比发现的功能缺口 | 当前实现 | 状态 |
| --- | --- | --- | --- |
| REC-01 | GPU 无法旋转显示器时，主机可能把不可用纹理或错误尺寸送入编码/截图；接收端也就无法得到屏幕方向的画面 | DXGI 捕获增加 CPU BGRA 旋转兜底；串流、截图、鼠标坐标、NV12 选择和 pointer-only 帧统一使用屏幕方向 | 源码完成，硬件待验收 |
| REC-02 | `voiceError` 是异步状态，原接收端只检查同步开启结果，主机拒绝、解码/播放失败和主机挂断可能没有任何提示 | 消费错误计数与原因变化，按“未能开启/出现问题/已结束”显示一次 SnackBar，并补中英文文案 | 完成 |
| REC-03 | 权限名称表缺少摄像头、终端、阻止输入、系统登录、会话切换、远程配置及未知位，权限变化可能显示为空或静默 | 补全全部协议权限位；保留未知位为“其他远程权限”，确保变化始终可见 | 完成 |
| REC-04 | 系统音频异步失败或停止时，用户只有打开统计层才能发现原因 | 对错误计数、原因变化和播放从启用变为停止做去重提示，补中英文文案 | 完成 |
| REC-05 | 合并多屏的 CPU 子屏始终上传到画布 `(0,0)`，副屏偏移后会画错位置 | CPU `UpdateSubresource` 使用子屏目标框；GPU 复制继续使用子纹理源框并由 `DestX/DestY` 定位 | 完成 |
| REC-06 | 外部共享读取器丢弃 CPU 子屏帧，合并捕获中的 `GdiCpu` 子屏无法进入最终画布 | 允许只读共享 CPU BGRA；`GdiCpu` 读取器明确发布无纹理、无设备的 CPU 帧；合并器上传后统一转 NV12 | 完成 |
| ACL D01–D03 | 受管准入、会话授权与生产 ACL 闭环尚未完成生产整链验收 | 票据签发/验证/持有证明、策略与传输凭证、20 秒续租、到期断开、撤权推送及越权拒绝已落地；真实主机单机回环已覆盖账号封禁、设备禁用/恢复、移组撤权、权限收窄、kid 退役/恢复和越权拒绝 | 真主机单机回环通过；生产部署待验收；保持部分完成 |

此前审计的 Viewer V1–V13 与 R01–R05 在本轮复核中保持已修复状态。本轮没有回退或覆盖已有的发送端/音频改动。

## 3. 已完成工作

### 3.1 旋转显示器兜底

- `src/capture/rotation.h`、`src/capture/rotation.cc`：新增 CPU BGRA 旋转和合并画布目标框工具。
- `src/capture/dxgi_capturer.cc`：GPU 旋转 pass 不可用时回读 HDR 后的 BGRA，旋转到屏幕方向后发布不可变 CPU 帧；此时不再声明不可用的 NV12 目标。
- 串流、截图、鼠标坐标和 pointer-only 更新沿用同一套屏幕坐标映射。
- 合并捕获可上传 CPU 子屏到正确的画布位置，再执行唯一的 NV12 转换。

### 3.2 接收端状态提示

- `app/lib/receiver_status_events.dart`：集中实现语音失败、系统音频失败和权限名称判定。
- `app/lib/pages/session_page.dart`：在会话状态更新中消费异步语音/音频错误并显示去重提示；权限变化使用统一名称表。
- `app/lib/viewer_bridge.dart`：补充摄像头权限位。
- `app/lib/l10n/en_session.dart`：补齐新增提示和权限名称的英文文案。
- `app/test/receiver_status_events_test.dart`：覆盖错误只提示一次、空原因、停止转换、全部权限位和未知权限位。

### 3.3 多显示器合并回归

- `src/capture/merged_capturer.cc`：CPU 上传使用子屏非零目标框；GPU 复制使用 `{0,0,width,height,1}` 源框，避免把画布偏移错误地用于子纹理。
- `src/capture/shared_capturer.cc`：CPU BGRA 也能跨只读共享边界发布，并只在桌面实际更新时递增桌面序号。
- `src/capture/gdi_capturer.cc`：显式 `GdiCpu` 读取器即使由合并器传入外部 GPU 设备，也强制走 CPU-only 发布。
- `tests/shared_capturer_test.cc`：在真实桌面上验证 CPU 子帧进入合并画布并完成 NV12 转换。

### 3.4 接收端结构化诊断日志

- `app/lib/receiver_status_events.dart`：新增 `viewer.system_audio.failure` 与
  `viewer.voice.failure` 两类稳定 JSON 事件。事件统一携带 schema/version、
  时间、失败类别与原因、触发条件、前后阶段，以及会话/视频编码、传输、
  RTT/帧龄、解码错误、音频设备/缓冲/欠播/延迟和语音计数器快照。
- `app/lib/pages/session_page.dart`：在仍存活的会话状态更新中调用同一个失败判定，
  每次新失败只输出一次；未变化的状态轮询不重复写日志。
- 默认通过 `debugPrint` 输出 `viewer-diagnostic <json>`，同时提供可注入 reporter，
  便于测试和后续接入本地日志归档。该实现不新增网络上报，不记录账号令牌、票据或输入内容。
- `app/test/receiver_status_events_test.dart`：覆盖两类事件选择、主机编码器/解码器/
  传输与延迟字段、前后阶段、三种音频触发、注入 reporter 去重和 JSON 编码。

### 3.5 合并 CPU 子屏缓存复用

- `src/capture/detail.h`：新增有界 `CpuFramePool`。只有池是该缓冲区唯一持有者时
  才复用其容量；只要共享读取器、合并器或编码器仍持有旧帧，就改用另一块缓存，
  保持 `FrameInfo::cpu_bgra` 发布后不可变的契约。同时持有的帧超过缓存上限时使用
  临时缓冲，池不会随帧数无限增长。
- `src/capture/gdi_capturer.cc`：CPU-only 捕获不再每帧新建并复制一个完整
  `std::vector`，改为从 DIB 写入可复用缓存。稳定尺寸下，单个软件兜底子屏预热后
  无需逐帧分配；它继续经共享读取器交给合并器，由合并画布完成唯一一次上传和 NV12 转换。
- GPU 子屏没有改走 CPU 缓存：其纹理仍由 `CopySubresourceRegion` 零拷贝进入合并画布。
- `tests/capture_transform_test.cc`：纯内存覆盖保留帧不复写、释放后复用、地址稳定和
  上限外临时缓冲；`tests/shared_capturer_test.cc` 在真实桌面 GDI CPU 读流上复核同一行为。

## 4. 验证结果

| 验证项 | 结果 |
| --- | --- |
| `chast-capture-transform-test` | 通过；CPU 90/180/270 度旋转、逐像素方向、非零合并画布偏移、GPU 旋转、HDR、NV12 缩放 |
| `chast-shared-capturer-test` | 通过；含外部 CPU 子屏进入合并捕获、画布上传和 NV12 转换 |
| `chast-desktop-stream-test` | 通过；含 CPU BGRA 编码与截图、共享捕获和编码恢复路径 |
| `chast-audio-dsp-test` | 通过；共享 AEC 参考、多生产者混合、欠载计数、回声消除和降噪 |
| `chast-host-voice-test` | 通过；语音建立/结束、权限撤销、播放失败恢复、DSP 显式关闭和原始麦克风回退 |
| `chast-audio-playout-test` | 通过；自适应 lead、时钟修正和停顿恢复 |
| `flutter test --reporter compact` | 737 项全部通过 |
| `flutter analyze --no-pub` | 无问题 |
| `flutter test test/receiver_status_events_test.dart` | 10/10 通过；覆盖结构化事件与去重 |
| `session_start_polling`、`session_display_streams` 定向测试 | 4/4 通过 |
| `chast-capture-transform-test` | 通过；CPU 帧池保留/复用/上限与缓冲内容校验全部通过 |
| `chast-shared-capturer-test` | 通过；真实 GDI CPU 读流保留帧不可变且释放后复用 |
| `capture_transform`、`shared_capture`、`shared_capturer`、`desktop`、`desktop_stream`、`viewer_display_streams` | 6/6 通过（47.41 s） |
| 定向 `git diff --check` | 接收端文件无新增空白错误；构建仅保留既存 `LNK4098` 链接警告 |

本次推送同时包含此前工作区中已完成的发送端/音频 DSP 改动。其原生定向测试与上面的接收端测试分别执行，未用 Flutter 结果替代原生验证。

自动化测试不能替代真实旋转显示器验收。REC-01 的硬件项继续保持未勾选。

## 5. 后续计划

### P0：真实设备与多屏矩阵验收

1. 在 90/180/270 度旋转显示器上分别验证单屏和合并多屏：画面方向、尺寸、鼠标坐标、截图、NV12 编码和指针更新。
2. 强制 GPU 旋转不可用，并在运行中模拟能力失效，确认自动切换到 CPU 路径且无需重连。
3. 使用主屏在原点、副屏位于正偏移和负偏移的布局，组合验证 DXGI、GDI CPU、HDR、不同缩放和 pointer-only 帧。
4. 完成语音拒绝/主机挂断/解码失败、系统音频停止、全部权限位变化的中英文端到端检查。

### P1：ACL 收口

1. 已将真实 `chast-host.exe`、真实代理和真实会话客户端接入当前 `chast-server`，完成单机回环验收；证据见 [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md)。
2. 接入生产 `chast-server`、真实反向代理/中继和双机 Flutter 查看器，复核票据到期与续租、权限收窄、撤权、会话切换、设备策略和账号权限上限交集；单机回环结果不外推为跨网结果。
3. PostgreSQL 13/14 集成、迁移、版本间恢复与双实例冷启动已通过，见 [PostgreSQL 13/14 真库验收](postgres-acceptance-2026-11-14.md)；仍需验证生产部署升级并在 PostgreSQL 上重跑 ACL 整链。在生产整链通过前，ACL 继续保持“部分完成”。

### P2：后续增强

1. ✅ 接收端异步失败已扩展到结构化诊断日志，输出主机侧编码/传输与本地解码/
   播放状态的前后快照；当前只写本地调试日志，远程采集仍需运维侧另行定义权限与留存策略。
2. ✅ 合并 CPU 子屏已接入有界缓存复用，稳定尺寸下避免逐帧整帧分配；GPU 子屏仍保持零拷贝路径。
3. 为真实硬件验收获批后补充设备矩阵、驱动版本和复现步骤。

## 6. 范围与约束

- 保留工作树中已有的发送端/音频修改，不回退、不覆盖。
- 不提交 `$null`、`.claude/`、`build-review-clean/`、截图、安装包及其他本地验收产物。
- 本文档记录源码与自动化状态；硬件验收完成后再更新 REC-01 的对应结果。
