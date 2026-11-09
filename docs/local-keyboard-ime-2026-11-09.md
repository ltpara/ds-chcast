# 本机键盘布局与输入法

2026-11-09。主会话与共享连接的显示器子窗口新增键盘模式菜单。继续使用已有 TextInput 消息，协议版本仍为 v60。

## 使用行为

- **物理按键**为默认模式：按键位置交给远端布局和输入法处理，保留原有 HID/扫描码路径。
- **本地输入法**使用本机布局和 Windows 原生组合、候选窗口；确认后发送 UTF-8 文字，主机按 Unicode 注入。普通文字不再同时发送物理按键。
- 导航和快捷键继续按键传输；输入法正在处理的回车、退格等留在本机。Ctrl/Shift+Space 留给本机输入法，Win+Space 属于远端快捷键。
- 主窗口记住模式；显示器子窗口从偏好初始化，可独立切换。聊天、终端等本地输入框获得焦点时恢复 Flutter 原来的输入上下文。
- 失焦、切换模式、关闭控制、停止/重连及检测到撤权时取消组合并释放按键。共享会话继续检查前台窗口、输入归属、显示流与会话代次，旧子窗口不能向新会话发送文字。
- 本地输入法或捕获不可用时提示重新聚焦或选择物理按键；不会悄悄把本地模式的文字退回物理输入。队列或权限拒绝文本时显示未发送提示。

## 实现与修复

- `local_text_input.{h,cc}` 在远程画布获得控制时关联自有 IMM 上下文，处理 GCS_RESULTSTR、WM_CHAR 与 WM_UNICHAR，支持 UTF-16 代理对，并避免重复的 WM_IME_CHAR。取消组合期间禁止重入提交；单次文本遵循现有 4096 字节协议上限。
- `keyboard_capture.{h,cc}` 区分普通文字、本地组合、快捷键与修饰键；保留按键 down/up 路由，不用 ToUnicode 探测字符以免消耗 dead-key 状态。普通键先经过 IMM，只有需要阻止本地系统动作的快捷键在低级钩子拦截。
- `viewer_bridge.{h,cc}` 将钩子与原生消息按次序衔接：每个需要本地处理的按键，等 TranslateMessage 产生的字符消息处理后才放行后续远端键。实际验收曾复现快速输入 `b → Enter → c` 变成 `Enter → b → c`，现已修正；快速拼音确认也不会因为钩子先于 STARTCOMPOSITION 而泄漏回车。
- Windows 字符消息不总保留注入的 extra-info。除直接检查消息标记外，还从前一个原生 keydown 保留来源，过滤本机主机注入的普通字符与 Unicode，下一次真实按键恢复正常输入。
- `ViewerEngine::SendText` 与按键、鼠标、释放共用一条有界待发队列，避免独立文本队列把确认文字排到随后快捷键之后。主机现有 QUIC 控制流与 InputInjector 顺序队列承接输入，不增加协议或连接。
- `SharedViewerSession::SendText` 验证当前窗口和权限，释放此前按住的远端状态，再发送确认文字。子窗口不会改变主窗口截图/录制的显示源。
- 显示流回归同时补齐了录屏收尾：取消录制屏幕的订阅时复用统一 close 路径，在落盘最后一帧后更新最终状态计数。原回归仍按即时写帧断言，现按实际末帧缓冲行为检查，并保留最终 MP4 恰好两帧的文件验证。

## 验证

- 复用既有测试文件和目标，没有新增 CMake 测试目标。相关 Flutter 检查 **51 项通过**，涵盖主/子窗口切换、偏好保存、原生捕获失败时禁止物理回退，以及原有显示菜单和订阅行为。`flutter analyze --no-pub` 无问题。
- 原有 `chast-keyboard-test` 通过，包含本地文字、组合回车/退格、快捷键释放、Ctrl+Space、AltGr，以及原有物理输入与注入回归。
- 最终原生回归 `keyboard`、`display_video`、`viewer_display_streams` **3/3 通过**（23.92 秒）；后者重新打开最终 MP4 验证两帧，并检查退订后的最终状态计数。测试用可执行文件已从产品 Release 目录移除。
- 临时 `build/local_ime_acceptance.cc` 使用产品原生组件验证 Win32 实际字符输入、emoji、中文确认、组合回车和 HIMC 恢复；真实微软拼音 `nihao + Space` 只确认一次“你好”。
- 临时 `build/local_flutter_ime_acceptance.cc` 链接产品 runner/core 与真实主机库，在实际 Flutter view 中启用生产键盘模式，使用 DXGI → OpenH264 → QUIC → D3D11VA 显示和单连接双窗口。主窗口与显示器子窗口的字符均到达真实 QUIC 主机；快速 `b → Enter → c` 顺序正确，Ctrl+C 没有额外文字。
- Flutter 原生窗口中的微软拼音 `nihao + Space` 通过 QUIC 只收到一次“你好”；快速 `ni + Enter` 在本机确认拼音字符串，没有向远端发送回车；未确认组合取消后没有文字到达主机。普通与 Unicode 注入回环过滤均通过，随后真实按键仍能发送。
- 同一验收继续验证输入归属、迟到释放拒绝、子窗口关闭后主窗口继续出帧、无新帧时的新窗口重放，以及父窗口关闭/旧代次拒绝。
- Windows Release 已构建并安装 Flutter bundle。沿用项目现存 LNK4098/LNK4199 链接警告，没有通过隐藏警告改变链接设置。

日志：`build/local-ime-flutter-test.log`、`build/local-ime-analyze.log`、`build/local-ime-native-build.log`、`build/local-ime-native-regression.log`、`build/local-ime-native.log`、`build/local-ime-flutter-native.log`、`build/local-ime-viewer-build.log`、`build/local-ime-bundle.log`。

原生集成验收通过生产 bridge 启用模式；菜单点击和偏好由 widget 检查覆盖。主机使用 `dry_run_input`，确认消息进入实际主机而不向本机其他应用注入；不能据此宣称已完成双机目标应用的输入效果验收。其他输入法、其他键盘布局/dead keys、混合 DPI 下候选位置、跨网络及安全桌面仍需实测。本轮提供两个明确模式，不等同于 RustDesk 全平台 Legacy/Translate/Map 行为；后续 v63 已补 Legacy 模式，见 [键盘 Legacy 模式](keyboard-legacy-mode-2026-11-10.md)。
