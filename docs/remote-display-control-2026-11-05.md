# 远端虚拟显示与隐私选择（2026-11-05）

本轮在 Windows 查看器、协议 42、HostServer 和 Windows 显示后端之间接通远端显示管理。对齐目标仍是本地 RustDesk 的功能；本页不表示所有 Windows 功能已完成。接手入口与后续顺序见 [Windows 开发交接](windows-development-handoff-2026-11-05.md)。

## 已接通的操作

查看器工具栏新增“虚拟显示器与隐私设置”：

- 查询实际启用的虚拟输出数量、驱动可用状态和当前会话选择的隐私方式。
- 在单个桌面查看者会话中选择 0–4 个启用的虚拟输出，恢复原显示布局。
- 根据主机返回的会话稳定 ID，单独停用/重新启用指定虚拟输出，包括 chast 所有的会话前已有输出；关闭最后一个活动桌面输出会被拒绝。
- 选择“遮罩”或“虚拟屏”隐私方式，再使用原隐私开关启停。选择仅影响当前会话，不写主机持久设置。
- 状态以主机结果为准；拒绝或失败显示原因。普通只读查看者可以查询，恢复自身布局在撤权后仍允许。

协议新增 `DisplayControl = 58` 和 `DisplayControlState = 59`。数量调整同时要求输入和隐私权限，隐私选择要求隐私权限；纯文件会话不能管理显示。主机再次校验权限与最新操作员限制，不能依靠查看器按钮限制代替主机授权。

## 拓扑、所有权与恢复

调整数量前先保存原始 CCD 路径、模式和位置，启动独立恢复进程并持有跨进程租约。原有物理输出继续保留，已有虚拟输出不会被删除；数量式操作不能低于操作开始前已有的虚拟输出数量，指定输出操作则允许暂时停用已有虚拟输出，断线后恢复。为了避免 usbmmidd 无目标的删除命令影响别的输出，减少数量使用 CCD 停用临时输出，不删除 PnP 设备。

因此“启用数量”指参与 Windows 桌面的虚拟输出，不是驱动分配的监视器设备数量。一次启用过四个输出后，设备可能保留以便再次启用，退出时恢复的是原显示布局。现有本机 CLI 的设备数量统计含义与此不同。

每个启用的虚拟输出均先验证 GPU 捕获、NV12 转换及硬件编码，再重建实际视频流并返回成功。拓扑变更前释放旧编码器和共享采集读者，避免新读者复用已经失效的 duplication 和缓存帧。没有新增 CPU/GDI 或软件编解码兜底；现有 GPU 内拷贝仍在，严格零拷贝尚未完成。

主动恢复、撤权和断线都恢复原模式/位置；操作中失败尝试恢复整个原布局，恢复失败保留恢复进程并关闭会话。管理期间阻止新桌面查看者进入；文件连接不占桌面独占权。临时显示管理与隐私激活暂时互斥，需先恢复布局或关闭隐私再切换。

## 验证证据

- `chast-viewer-probe --display-control`：真实 ViewerEngine、认证 QUIC、HostServer、硬件视频；查询、隐私实现选择、撤销隐私/输入权限、撤权后恢复请求。测试不会修改显示布局。
- 同一用例还验证 ViewerEngine 配置自动锁屏、单会话监听模式退出后恰好调用一次锁屏回调；使用替代回调，没有锁住工作站。
- `chast-viewer-probe --display-control-live`：本机 Intel UHD 730 / usbmmidd 实测通过 1→2→4→1→0 输出，结果与实际 CCD 输出数量一致，视频继续输出。远端选择虚拟屏隐私后仅一个虚拟输出处于活动状态，关闭后恢复物理屏；管理/隐私互斥、撤权恢复、断线恢复及原模式/位置核对均通过。日志 `build/align-display-control-live-retry.log`。
- 首轮实机发现旧共享采集资源被复用，导致 `AcquireNextFrame 0x887A0001`，视频中断。已修复资源释放顺序后重跑通过；首轮日志 `build/align-display-control-live.log` 保留，失败后的测试等待已终止，不能将首轮记为通过。
- Flutter 全量 580 项通过，静态分析无问题；协议测试覆盖非法动作、数量、隐私类型、状态标志、长度和 UTF-8；界面测试覆盖撤权后禁用修改但保留恢复入口。
- C++ 全量 Release、Flutter Windows Release 构建通过；最终 CTest 定向回归 **11/11，通过用时 28.89 秒**，涵盖协议、主机配置/安全、锁屏策略、输入屏蔽、多会话、虚拟显示、隐私恢复、真实 ViewerEngine 显示控制和文件/终端路径。日志 `build/align-display-control-final-regression.log`、`build/align-display-control-full-build.log`、`build/align-display-control-viewer-build.log`；不是全量 CTest 验收。

## 尚未闭环

- 指定输出的远程停用/重新启用和既有 chast 所有输出的临时管理已接通；底层使用 CCD 停用，不提供按目标卸载 PnP 监视器设备。非 chast 所有的驱动仍拒绝远端修改。
- 多桌面查看者共享管理，以及临时显示与隐私模式无缝切换。目前明确拒绝冲突操作。
- 扩展管理模式现已通过强杀、Job 关闭/禁止 breakaway、恢复进程强杀场景；仍缺扩展模式 GPU 故障注入、真实 SCM、安全桌面、多 GPU、物理热插拔矩阵。
- 普通桌面会话（尚未启用隐私或显示管理）对本机驱动 CLI 的统一占用协调、驱动正式分发与干净机器安装矩阵。
- 实际 Windows 自动锁屏及解锁重连仍待验收。

## 指定虚拟输出增量（协议 42）

`DisplayControlRequest` 增加 32 位 `target`，`ToggleOutput` 携带启用/停用意图；其他动作的 target 必须为 0。状态返回最多 4 个 `(id, active)` 条目，拒绝重复/零 ID、非法标志、过长/截断消息和不合法 UTF-8。ID 绑定当前会话的 Windows 监视器接口身份，不绑定 DISPLAYn 或显示列表下标；实际输出消失后旧 ID 不再复用。重新连接应使用新会话查询结果。

主机在操作前重新枚举并验证 ID。未知 ID 在修改拓扑前拒绝，不恢复或替换其他正在使用的布局。指定操作同时要求输入和隐私权限，并校验驱动安装所有权。重新启用仅选择该目标的空闲 CCD 源；停用主输出时将剩余源移到原点。当前正在采集的虚拟输出存活时按其接口身份重建视频，已停用时明确选择仍存活的主输出，避免 DISPLAYn 复用造成错屏。

验证记录：

- 真实 ViewerEngine 实机：4 屏中的中间屏停用、重复停用、相同 ID 重新启用、未知 ID 拒绝，其他输出的 ID/启用状态保持正确；切到虚拟屏后逐个停用所有虚拟屏，GPU 视频切回存活桌面。数量切换、隐私启停、撤权/断线恢复也通过。日志 `build/align-display-target-captured-live.log`。
- 扩展管理验收 **5/5**：会话前已有虚拟屏的暂时停用/重启用/断线恢复；禁止 breakaway 的 Job 拒绝修改；恢复子进程强杀后主机恢复且 GPU 视频继续；主机强杀；服务同配置 Job 关闭。每项检查原输出、模式、位置及租约释放，未触发监督进程紧急恢复。日志 `build/align-display-management-failures-detail.log`。不是实际 SCM 服务验收。
- 此矩阵首次运行 `management-existing` 失败且当时未收集子进程输出；补齐继承日志句柄后复测 5/5 通过。首轮失败原因尚未确定，保留 `build/align-display-management-failures.log`，不能将首次运行记为通过。
- Flutter 定向 **28 项**通过，静态分析无问题；C++ 全量 Release 与 Windows 查看器 Release 构建通过。协议 42 必须配套更新两端。日志 `build/align-display-target-flutter.log`、`build/align-display-target-analyze.log`、`build/align-display-target-full-build.log`、`build/align-display-target-viewer-build.log`。
- 最终协议 42 的 CTest 定向回归 **11/11 通过，32.43 秒**，覆盖协议、主机配置/安全、锁屏策略、输入屏蔽、多会话、虚拟显示、隐私恢复、真实 ViewerEngine 显示控制和文件/终端路径。日志 `build/align-display-target-final-regression.log`。本轮未将上一轮 Flutter 全量 580 项重记为新执行。
