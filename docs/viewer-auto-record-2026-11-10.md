# 查看端出站自动录制（2026-11-10）

本轮补齐 G11 的查看端部分：出站自动录制开关、录制容量守卫、失败提示和可选的保留策略。对照仍使用本地 RustDesk `7423dced37845c48093b6f71bd82e4a16cc8c79d` 的 `allow-auto-record-outgoing` 行为（默认关闭）。主机入站 `record_incoming` 的既有实现见 [主机自动录屏](host-recording-2026-11-09.md)，两条链路互不依赖。

## 使用方式

远程会话工具栏的录制按钮新增长按菜单：

- **每次会话自动录制**：默认关闭。开启后，每个直播阶段在收到首帧且主机授予录制权限时自动开始录制一次。
- **保留全部录制**（默认）/ **保留最近 10 个录制** / **保留最近 30 个录制**：只对查看器自己命名的 `chast-*.mp4` 生效，且只在一个录制完全结束后清理。

录制按钮提示会随状态变化（已开启自动录制时显示“已开启自动录制，长按设置”）。手动停止录制后，本直播阶段不再自动开始；重新连接形成新的直播阶段后恢复。录制失败通过右下角提示“录制失败：{error}”显示一次，相同错误不重复弹出，成功开始录制后清除已见错误。

存储位置沿用原生查看器既有的 `SetRecording` 目录；未指定时由引擎选择默认录像目录（Windows 为 `%USERPROFILE%\Videos\<应用名>`，与 RustDesk `video-save-directory` 的默认位置一致）。账号录制上传逻辑不变：结束时排队的文件仍按既有设置上传，保留策略会保护排队中的文件。

## 实现

### 偏好

- `app/lib/view_prefs.dart:49`：`autoRecord`（键 `view_auto_record`，默认 `false`）对应 RustDesk `allow-auto-record-outgoing`，是查看器本地共享偏好，不按设备区分；`recordKeep`（键 `view_record_keep`，默认 `0` 表示保留全部，合法范围 0–1000，菜单预设 10/30）。
- 读到类型错误或越界值一律回退默认值，写入时同样归一化。

### 自动开始与失败提示

- `app/lib/pages/session_page.dart:611`：`_watchRecording` 在直播阶段切换时重置“已尝试/已手动抑制”状态；仅在直播、非重连、未在录制、本阶段未尝试、未被手动停止、已有首帧且 `acceptsRecording`（主机授予 `kPermissionRecord` 且引擎支持）时调用一次开始。
- `app/lib/pages/session_page.dart:645`：菜单开启自动录制时立即尝试开始（同样受首帧和权限约束）；关闭不停止正在进行的录制。
- 手动停止在 `app/lib/pages/session_page.dart:1996` 设置抑制标记；重新开始录制时清空已见错误。
- 失败提示只对新的 `ViewerStatus.recordError` 文本弹一次，错误清空后重置。

### 容量守卫

- `app/windows/viewer/viewer_engine.cc:140`：开始录制要求录像目录至少有 512 MiB 可用空间，否则拒绝开始并报 `not enough free space in the recording folder (512 MiB required)`。
- 录制中每 2 秒检查一次（`app/windows/viewer/viewer_engine.cc:2403`），低于 64 MiB 时停止录制并报 `recording stopped: the recording disk is almost full`，视频流继续。
- 查询磁盘空间失败时按“允许”处理，避免文件系统怪癖阻断录制；该检查基于已压缩访问单元写盘，不改变零拷贝媒体路径。

### 保留策略

- `app/lib/recording_retention.dart`：`pruneRecordings(directory, keep, protect:)` 按文件名时间戳 `chast-YYYYMMDD-HHMMSS(-序号).mp4` 分组，新组在前，保留最新 `keep` 组，旧组整组删除；有任一受保护文件（仍在录制、排队上传、正在上传）时跳过整组。
- `app/lib/pages/session_page.dart:578`：录制完全停止后（不是每次分段）触发清理；只清理完成文件所在目录，忽略非查看器命名的文件；删除失败静默跳过，下次录制结束时重试。
- 默认 `0` 保留全部，不自动删除，与主机入站录制“不自动删除旧录像”的策略一致。

### 工具栏与文案

- `app/lib/widgets/remote_toolbar.dart:571`：录制按钮长按菜单，`CheckedPopupMenuItem` 显示当前开关与保留档位；无回调时不弹出（测试替身场景）。
- `app/lib/l10n/en_session.dart:240`：四条菜单项与“录制失败：{error}”的英文文案。

## 边界

- `hide-recording-button` 是 RustDesk 的受管客户端高级设置（隐藏按钮但不禁止录制），属于受管策略范畴，本轮不实现，也不把它折算成 G11 缺口。
- 集中留档仍由受管主机负责并报告失败，查看器只做本地录制、上传与本地保留。
- 容量守卫只覆盖查看器本地录制，不限制主机入站录制目录；保留策略只处理查看器命名文件。
- 自动录制是查看器共享偏好，不是按设备偏好；关闭程序后保持上次选择。

## 验证

| 层级 | 结果 |
| --- | --- |
| 已实现 | 偏好、自动开始、容量守卫、失败提示、保留策略、长按菜单、英文文案 |
| 已构建 | 本机时间 2026-11-10 05:47 Release 重建通过；`chast-display-video-test`（含 `viewer_engine.cc` 容量守卫）链接成功 |
| 自动化通过 | `flutter analyze` 无问题；`view_prefs_test.dart`、`recording_retention_test.dart`、`display_menu_test.dart` 及全量 Flutter 套件 698/698 通过（2026-11-10） |
| 真实链路通过 | 待人工验证：需要真实会话、主机录制授权和磁盘满场景 |
| 待人工验证 | 长按菜单目视、实际自动开始、512/64 MiB 阈值行为、10/30 档清理结果 |

全量原生 ctest 在并行运行 Flutter 全量套件时曾出现 3 项时序失败（`display_subscriptions` 未构建、`quic_recording_hevc`、`quic_weak_network`）；空闲时逐项复跑 3/3 通过，`display_subscriptions` 的缺失产物是 `chast.net` 票据符号未链接，已补 `chast.access` 依赖。随后在无并发负载下重跑 Release 全量 ctest：**69/69 通过**（914.66 秒）。
