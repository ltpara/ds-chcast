# Windows 开发交接（2026-11-05）

本页是 Windows 功能对齐工作的接手入口。状态快照对应当前代码提交
`f5bc719`，分支为 `feat/windows-session-display-20261105`，协议版本为
42。详细功能审核见 [Windows 功能缺口复核](windows-functional-audit-2026-11-05.md)，
显示管理实现见 [远端虚拟显示与隐私选择](remote-display-control-2026-11-05.md)。

## 当前目标与约束

- 先完成 Windows，再推进 Linux、macOS 和移动端。
- 视频采集、转换、编码、解码、呈现均不得加入 CPU/GDI 采集或软件编解码
  fallback。硬件不支持时必须明确报错。
- 目前没有 CPU 像素回退，但**严格纹理零拷贝尚未完成**。共享采集仍有 GPU
  `CopyResource`，FFmpeg 编码仍有 GPU `CopySubresourceRegion`，oneVPL 使用
  `IMPORT_COPY`；Intel UHD 730 实机记录为 `shared_import=0`。后续工作应先保持
  这一事实表述，再逐步消除 GPU 内拷贝。
- 当前对齐本地源码的可证实工作流，不要求协议互通，也不把 RustDesk Pro 或未
  提供的 Web 核心自动计入基线。

## 已接入且有调用链的能力

| 能力 | 当前状态 | 主要入口 |
| --- | --- | --- |
| 多查看者与多窗口 | 主机最多 8 个认证会话，查看器每连接独立窗口，逐会话权限、聊天、文件、终端和退出 | `src/net/quic.cc`、`src/host/host_server.cc`、`app/lib/session_window.dart` |
| 共享 GPU 采集 | 同一输出共享 DXGI duplication，各查看者独立 GPU 缩放与编码 | `src/capture/shared_capturer.cc` |
| 独立显示器窗口 | 可从显示器菜单为指定屏幕打开独立 GPU 会话 | `app/lib/pages/session_page.dart` |
| 双终端标签 | 每会话最多两个 ConPTY，独立输入、输出、尺寸、历史和关闭 | `app/lib/widgets/terminal_tabs.dart`、`src/host/terminal_relay.cc` |
| 本地输入屏蔽 | 独立权限、状态和失败原因；撤权、断连、桌面切换释放按键与鼠标 | `src/win/input.cc` |
| 会话结束锁屏 | 默认关闭，要求输入权限；最后一个桌面查看者退出后触发，可记录回调验证 | `src/host/host_server.cc`、`app/lib/widgets/remote_toolbar.dart` |
| 虚拟显示隐私 | 遮罩或虚拟屏；原拓扑快照、GPU 预检、独立恢复进程和恢复租约 | `src/win/display_privacy.cc`、`host/display_guard_main.cc` |
| 远端显示管理 | 查询、0 至 4 个输出、稳定 ID 指定停用/启用、恢复布局、隐私实现选择 | `include/chast/display_privacy.h`、`src/host/host_server.cc` |
| 文件和辅助通道 | 纯文件会话、文件续传、终端、摄像头、端口转发、聊天和剪贴板已有入口 | `src/clipboard/file_transfer.cc`、`src/host/host_server.cc` |
| 中继复用 | 直连、UDP 中继、TCP/WS 已按会话隔离，慢连接只关闭自身路由 | `server/internal/agent/multiplex*.go`、`server/internal/relay/multiplex*.go` |
| 触摸与触控板 | 点击、拖动、双指平移、捏合缩放接入独立协议和权限检查 | `app/lib/widgets/remote_view.dart`、`src/win/input.cc` |

## 剩余功能缺口

仍按建议优先级排列；W07 已实现但需实机验收，因此不计入 13 项功能缺口。

| 编号 | 优先级 | 后续工作 | 完成标准 |
| --- | --- | --- | --- |
| W01 | P1 | 会话内远端请求提权 | 查看器发起、主机同意、UAC 后重连并保持已授权状态；拒绝和超时有明确结果 |
| W02 | P1 | Windows/RDP 用户会话列表与选择 | 服务枚举、权限校验、选择目标会话、断开后重连；验证多用户和 RDP 条件 |
| W04 | P1 | 独立终端、端口转发、摄像头连接 | 不依赖桌面 GPU 建链；各模式有独立配额、权限、断连清理和真实 QUIC 测试 |
| W05 | P1 | 终端断线保留与恢复 | 显式启用、绑定已认证设备、service ID 续接、输出回放和超时/撤权清理 |
| W06 | P1 | 多查看者显示管理及隐私组合 | 明确所有权与冲突语义；临时显示和隐私可安全切换；失败、撤权和并发均有测试 |
| W10 | P2 | 跟随远端光标/前台窗口自动切屏 | 查看器选项、主机观察器、稳定输出身份、去抖/冷却、手动切屏覆盖和 GPU 流重建 |
| W08 | P2 | 新屏幕窗口的后续授权 | 同一已授权会话内派生授权或订阅，避免一次性口令造成重复认证 |
| W09 | P2 | 协助者光标标识 | 多查看者身份、权限、光标绘制和断开清理 |
| W11 | P2 | 完整相对鼠标模式 | 捕获、无限相对移动、失焦/退出恢复和 UI 状态完整 |
| W12 | P2 | 远程打印 | 协议、队列、驱动适配、查看器入口和真实打印端到端验收 |
| W13 | P2 | 更多硬件编码与色度 | 仅在端到端 GPU 路径成立时增加格式；能力不足明确报错，不引入 CPU fallback |
| W03 | P2 | 控制双方互换及切回 | 双方同意、角色变更、重连和撤销路径完整 |
| W14 | P2 | 更多产品语言 | 保持占位符和格式，覆盖完整界面及界面测试 |

## 推荐开发顺序

1. **严格零拷贝基线**：先测量每个 GPU 拷贝点的必要性，优先让采集纹理直接进入硬件编码器，
   再处理跨适配器和多查看者读者生命周期。此阶段不扩展 CPU fallback，也不重复声称已经零拷贝。
2. **W10 跟随切屏**：这是上一轮确定的下一个功能项。加入查看器和主机状态后，协议升到 43；
   自动切屏必须使用监视器稳定身份，处理手动覆盖、隐私、撤权、合并视图、失败 GPU 重建和
   冷却/重试上限，并验证视频持续输出。
3. **W01 / W02**：完成提权和 Windows 用户会话选择，这两项直接影响高级协助与无人值守场景。
4. **W04 / W05**：拆分独立辅助连接，再实现可恢复终端；两者都需要独立认证与资源配额。
5. **W06**：补齐多查看者显示所有权、隐私组合和并发失败语义。
6. **W08、W09、W11、W12、W13、W03、W14**：按实际使用频度推进。W13 始终服从硬件
   路径约束，W14 只补完整翻译，不用空资源凑数。
7. **发布门槛**：完成实际 SCM 服务、冷启动无人登录、注销、锁屏/UAC、硬崩溃、跨 NAT、
   TCP/WS、不同 GPU、多适配器、HDR/热插拔、双机音频、触控硬件和干净机器驱动安装矩阵。

## 已完成的验证

- 代码快照前的 C++ Release 全量构建和 Flutter Windows Release 构建通过；Flutter 静态分析无问题。
  Flutter 协议 42 定向 28 项通过。
- 协议 42 定向 CTest **11/11** 通过，用时 32.43 秒；包含协议、主机配置/安全、锁屏策略、输入
  屏蔽、多人会话、虚拟显示、隐私恢复、真实 ViewerEngine 显示控制和文件/终端路径。
- 真实 ViewerEngine、QUIC、GPU 和 usbmmidd 实机完成 1→2→4→1→0 输出、指定输出停用/重启用、
  未知 ID 拒绝、隐私切换、撤权和断线恢复。
- 显示管理故障矩阵 **5/5** 通过：已有输出接管、禁止恢复进程 breakaway 时拒绝拓扑变更、
  恢复进程被强杀、主机被强杀和 Job 关闭。
- 验证日志位于被忽略的 `build/`：
  `align-display-target-full-build.log`、`align-display-target-viewer-build.log`、
  `align-display-target-flutter.log`、`align-display-target-analyze.log`、
  `align-display-target-final-regression.log`、`align-display-target-captured-live.log` 和
  `align-display-management-failures-detail.log`。这些日志不进入 Git，换机器后需重新运行。

## 复现检查

在仓库根目录执行：

```powershell
cmake --build build --config Release
Push-Location app
flutter analyze
flutter test test/display_menu_test.dart test/terminal_tabs_test.dart
flutter build windows --release
Pop-Location
ctest --test-dir build -C Release -R '^(protocol|host_settings|host_security|lock_after_session|block_input|multi_session|virtual_display|display_privacy|viewer_display_control|viewer_folder_download|viewer_files_only)$' --output-on-failure
```

硬件显示测试会改变本机显示布局，只能在明确授权且确认物理输出和恢复进程可用时运行：

```powershell
build\Release\chast-viewer-probe.exe --display-control-live
build\Release\chast-display-privacy-live.exe --run-management
```

## 已知未闭环项

- 严格纹理零拷贝未完成，当前只能表述为“GPU 路径、无 CPU 像素回退”。
- W07 实际 Windows 锁屏/解锁重连尚未验收；主机硬崩溃不保证执行锁屏。
- SCM 服务全链路、安全桌面、无头重启、真实双机/跨网及多设备矩阵尚未验收。
- 显示管理仍拒绝多查看者冲突操作，临时显示与隐私不能无缝组合。
- 虚拟输出通过 CCD 停用，不提供按目标卸载 PnP 监视器设备。
- 普通桌面会话没有始终持有恢复租约，管理员直接操作驱动 CLI 仍缺少统一会话占用检查。
- 音频回归历史上出现过阈值波动，最近通过不能代表根因已经解决。