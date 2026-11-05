# Windows 虚拟显示器与隐私模式（2026-11-05）

本轮补齐 Windows 虚拟显示隐私，并在本机 Intel UHD Graphics 730 上完成驱动安装、数量/分辨率、GPU 编解码、隐私切换及进程/Job 强制终止恢复验收。保持 GPU 捕获、NV12 转换及硬件编码，无 CPU/GDI fallback。本机编码器报告 `shared_import=0`，即使用 GPU 内拷贝，不能称为纹理完全零拷贝。其他显卡、安全桌面与跨机器矩阵仍有边界，不能据此宣布与 RustDesk 完全对齐。

## 使用入口

- 主机“安全设置 → 高级 → 隐私方式”：黑色遮罩（默认）或虚拟屏（关闭物理显示器）。还需要开启“允许隐私模式”，查看器沿用已有隐私按钮。
- 配置文件：`privacy_mode_impl=virtual_display`；权限仍由 `privacy_mode=1` 和已有权限掩码控制。无效实现名称会拒绝请求，不静默改用遮罩。
- 托盘：启用虚拟显示器（按配置数量）、停用虚拟显示器。远程会话或隐私恢复进行中禁止操作。
- `virtual_display=auto/on` 现在由公共 HostServer 启动路径执行，托盘与服务一致；子会话不重复修改拓扑。
- 发布时必须同时携带 `chast-display-guard.exe`。CMake 的 host/service 目标自动构建该程序，Windows 打包脚本要求并复制它。

## 切换与恢复

1. 获取会话级命名互斥对象的独占租约，阻止其他 chast 进程同时管理显示器。保存 CCD 活动路径、模式、位置及主屏信息。
2. 启动独立恢复进程，传入限定继承的父进程句柄、只读快照映射、就绪事件、恢复事件及租约句柄。等待它验证并确认快照后才允许修改显示器。
3. 识别 usbmmidd 监视器的设备接口及 PnP 父设备硬件 ID/服务；不依赖可本地化的显示名称。必要时启用一个由 chast 安装的虚拟监视器，或重新挂接上次恢复时脱离桌面的监视器。
4. 对指定虚拟屏实际完成 DXGI 捕获、GPU NV12 转换和硬件编码测试；不支持则恢复并返回具体错误。
5. 使用 `SetDisplayConfig` 保留一个虚拟显示目标，设置首选分辨率和原点；不设置 `SDC_SAVE_TO_DATABASE`。重新解析设备接口、重建视频链路、验证编码，更新 StreamInfo 并请求 IDR。
6. 关闭、撤权或会话结束时，恢复进程应用原配置；正常关闭后重建原屏幕/合并视图的视频流。CCD 拓扑恢复后 DXGI/DWM 可能短暂返回 `E_ACCESSDENIED`，现已增加有界的重新枚举与 GPU 链路重建重试，不切换 CPU 捕获。恢复失败保留租约和快照并持续重试。
7. 主机崩溃或服务终止使父进程句柄触发，恢复进程执行同样的恢复。服务 Job 允许该恢复子进程显式脱离，因此关闭 Job 不会连同恢复程序一起杀死。外部 Job 不允许脱离时，隐私启用在物理屏关闭前失败。
8. 活动期间检查虚拟屏拓扑及恢复进程存活；检测到显示热插拔/配置改变或恢复进程退出，尝试恢复并结束会话。成功恢复会再次枚举并核对目标集合。原物理屏拔出时尝试恢复仍连接的原屏幕；全部不可用时等待重连。

不卸载其他程序的驱动，也不依靠 usbmmidd 的无目标编号删除命令移除临时屏。退出后原活动拓扑恢复，新增虚拟监视器可保留在 PnP 中但脱离桌面；下次启用会重新挂接，也可在托盘手动停用。虚拟隐私期间拒绝切换屏幕和临时分辨率操作，避免与恢复快照冲突；首选分辨率在主机高级设置中选择。

## 本机情况与边界

初始扫描发现 Intel UHD Graphics 730、2560×1440 物理屏以及 ToDesk 虚拟显示适配器（硬件 ID `Root\tdIdd`）。已增加该硬件 ID 的冲突检测。

后续处理（2026-11-05，用户授权停止/必要时卸载 ToDesk）：确认 ToDesk 没有运行进程、服务已停止，但 `oem38.inf` / `tdIdd` 仍加载。先将驱动包与原服务配置备份至 `build/driver-backups/todesk-20261105/`，再禁用 `ToDesk_Service` 并通过 PnP 卸载该虚拟显示驱动包，返回码 0。保留 ToDesk 应用本体。随后安装了 usbmmidd 2.0.0.1，设备 `ROOT\DISPLAY\0001` 状态 OK。验收结束保留驱动，停用全部虚拟监视器，物理拓扑已恢复；`build/align-vdisplay-final-state.log` 报告 `installed active=0 max=4`。

官方 ZIP 固定 SHA-256 为 `629b51e9944762bae73948171c65d09a79595cf4c771a82ebc003fbba5b24f51`。辅助 EXE 无签名，因此仅允许完整匹配内置六个文件哈希的已审阅包，并继续验证 CAT、x64/Win32 DLL 签名、包 ACL 和只读文件锁；篡改 manifest 不能扩大授权。包位于 `build/Release/drivers/usbmmidd_v2`，只用于本机主动安装，未制作含驱动的发布包；再分发条款状态见 `THIRD-PARTY-NOTICES.md`。

尚需其他设备或专门环境补充的矩阵：

- 物理单屏/多屏、扩展/镜像、旋转、不同适配器及首选分辨率。
- 真实网络断开、SCM 服务进程终止。当前已验收本地 QUIC 查看器退出、主机进程强制终止，以及与服务相同 `KILL_ON_JOB_CLOSE | BREAKAWAY_OK` 配置的 Job 关闭；后者不等同于实际 SCM 服务全链路测试。
- 外部程序安装的 usbmmidd，以及恢复时拔出/重新插入物理屏。已有临时屏与第二次启用已通过本机验收。
- 锁屏、UAC、登录/注销、系统重启、外部 Job 限制及 GPU 设备重置。
- 不支持虚拟输出 DXGI 或硬件编码时拒绝激活，验证无 CPU 回退且原桌面仍可用。

恢复程序本身被强制终止且主机同时崩溃、断电、驱动内部持久配置行为，不由单个用户态恢复进程保证。系统重启与驱动异常后的恢复须实机验证；本轮不作安全桌面全覆盖保证，也不自动屏蔽本地键鼠（它有独立权限）。

## 验证

新增 `display_privacy` 测试覆盖恢复就绪、GPU 预检、隔离、流提交、完整性检查各阶段失败，异常抛出、断连析构恢复、恢复失败重试、重复启停、租约可见性，以及跨适配器拓扑构造和非法快照/句柄拒绝。`host_settings` 覆盖方式解析、持久化和错误配置。`multi_session` 验证双查看器时隐私请求在任何显示修改之前被拒绝。

实机程序 `chast-display-privacy-live.exe --run-hardware` 与 `--run-failures` 显式启用，未注册到普通 CTest。监督进程在每个场景核对原 CCD 目标、分辨率、位置与旋转、租约释放；失败时另有监督进程恢复路径。先验证恢复子进程确认快照及父进程突然退出，再进行真实隔离。

2026-11-05 实机记录：

- `build/align-vdisplay-install-live.log`：初次传入不支持的 `install --count 0` 被拒绝且未修改设备；改用 `--count 1` 安装成功，退出 0。
- `build/align-vdisplay-live.log`：首轮发现退出隐私后 `DuplicateOutput 0x80070005` 导致会话结束；拓扑恢复成功。增加有界 GPU 重建重试后再验收。
- `build/align-vdisplay-live-full.log`：五个场景全部通过：恢复程序就绪/父退出、provider 数量和模式、会话开关/第二次开启/撤权/断线、强杀主机、关闭服务同配置 Job。
- `build/align-vdisplay-live-failures.log`：四个异常场景全部通过。GPU 预检故障注入和隔离后流提交故障注入均使用真实 Windows 后端，恢复原拓扑并清除恢复状态；故障注入不等于已覆盖不支持的 GPU。真实禁止 breakaway 的 Job 下恢复程序创建返回错误 5，隐私在物理屏关闭前拒绝，GPU 会话继续。单独强杀当前测试主机的恢复子进程后，主机检测到退出，使用自身快照恢复原物理屏并关闭会话。所有场景都未触发监督进程的紧急恢复路径。
- provider 覆盖数量 1→2→4→1→0；对每个活动虚拟屏实际完成 DXGI→GPU NV12→oneVPL AVC→GPU 解码，确认纹理无 CPU 访问、无 CPU 解码帧。模式 1920×1080、2560×1440、3840×2160 全部通过。编码器 `mfx-gen (VDENC AVC)`，`shared_import=0`。
- 隐私期间 CCD 核对仅有一个经 PnP 识别的虚拟目标；退出、撤权和断线后原物理目标/模式/位置恢复，正常退出与撤权后视频继续。强杀场景在关闭物理目标且收到 GPU 画面后才执行，恢复过程不依赖父进程析构。
- `build/align-vdisplay-reviewed-package-test.log`：真实包正向验证及安装器、INF、CAT、DLL 篡改拒绝全部通过；同步改写 manifest 仍被拒绝，未执行任何篡改文件。
- `build/align-vdisplay-validation-full-build.log`：本轮全量原生 Release 构建通过。
- `build/align-vdisplay-failures-build.log`：增加异常场景后的实机验收程序构建通过；此阶段未修改产品代码，沿用下列产品回归结果。
- `build/align-vdisplay-validation-regression.log`：本轮回归 CTest **9/9 通过，48.43 秒**，包含 display_privacy、virtual_display、host_settings、host_i18n、host_security、block_input、multi_session、viewer_files_only、quic_stream_options_ffmpeg；未放宽音频阈值。

前一轮回归记录（保留历史，当前实机结果以上文为准）：

- 全量原生 Release 构建通过：`build/align-vdisplay-full-build-final.log`。设置页尺寸调整后的主机再次构建通过：`build/align-vdisplay-host-final.log`。
- 定向 CTest **8/8 通过，17.03 秒**：display_privacy、virtual_display、host_settings、host_i18n、host_security、block_input、multi_session、viewer_files_only，日志 `build/align-vdisplay-regression-final.log`。多人测试还验证持有恢复租约时单人隐私请求明确失败，原会话仍可获得 GPU 解码画面。
- 扩展回归首轮 **8/9**：此前已存在的 `quic_stream_options_ffmpeg` 音频丢帧断言再次失败（errors=0、dropped=14），视频检查通过；原阈值不变，单独复测通过（31.85 秒）。日志 `build/align-vdisplay-regression.log` 与 `build/align-vdisplay-stream-options-retest.log`。根因未确定，不记作首轮全绿。
- `--vdisplay status` 只读烟测退出 0，正确报告 `Root\tdIdd` 冲突：`build/align-vdisplay-status.log`。
- `git diff --check` 在保留 CRLF 的配置下通过。前一轮未改动驱动或拓扑，本轮实机验收已按上述记录实际执行并恢复。
