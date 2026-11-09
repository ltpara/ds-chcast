# 键盘 Legacy 模式（协议 v63）

2026-11-10。查看端新增第三种键盘模式“传统按键”（Legacy），与已有“物理按键”（Map 方向）和“本地输入法”（Translate 方向）并列。协议升到 v63，两端需要同时升级。

## 使用行为

- **物理按键**：发送 HID usage，按物理位置映射，远端布局决定字符（原有默认）。
- **传统按键**：发送本机低级键盘钩子报告的 Windows 虚拟键码（VK），主机用 `SendInput` 注入同名按键，由主机布局和输入法解释。适合远端布局与本机一致、但希望远端输入法/死键/AltGr 直接参与的场景；本机不翻译字符。
- **本地输入法**：本机组合、候选窗口和 Unicode 提交，主机按文字注入（见 [本机键盘布局与输入法](local-keyboard-ime-2026-11-09.md)）。
- 三种模式互斥。切换或停止控制前先释放已按住的远端按键；模式按设备保存，显示器子窗口独立选择。
- 锁定键（Caps/Num/Scroll）仍留在本机切换，主机按位图差异补一次敲击同步；修饰键按各自的模式记录和释放，避免切模式后卡键。
- 菜单标题为“传统按键”，副标题说明“发送虚拟键码，由远端布局和输入法解释”。

## 实现

- 协议：`InputKind::LegacyKey = 6`，复用固定 24 字节 `InputEvent` 的按键槽位承载虚拟键码，动作仍为 Down/Up；解析拒绝 0、超过 `0xFF`、非法动作和非按键字段非零，注入层另外拒绝 `VK_PACKET`/`VK_PROCESSKEY`。见 `include/chast/protocol.h`、`src/net/protocol.cc`。
- 主机：`make_legacy_keyboard_input` 只设置 `wVk`，不设 `KEYEVENTF_SCANCODE`；扩展键补 `KEYEVENTF_EXTENDEDKEY`。`apply_legacy_key` 维护独立的按住表，`sync_modifiers` 逐键按原模式释放/按下，锁定键用 `lock_bit_for_vk` 对齐。注入仍带本进程标记，回环过滤不变。见 `include/chast/input.h`、`src/win/input.cc`。
- 查看端：`KeyboardCapture::SetLegacy` 让钩子改发 `LegacyKey`，重复按下去抖、AltGr 合成 Ctrl 过滤、快捷键逃逸和锁定键本地处理沿用原有逻辑。`ViewerBridge::SetKeyboardMode` 接受 `physical`/`legacy`/`local`，切换前排空远端输入并在失败时回退物理模式。见 `app/windows/viewer/keyboard_capture.{h,cc}`、`app/windows/viewer/viewer_bridge.{h,cc}`。
- Flutter：`ViewerKeyboardMode` 枚举、按设备偏好（`legacy` 持久化，未知值回退 `physical`）、主会话与显示器子窗口菜单和中英文文案。见 `app/lib/viewer_bridge.dart`、`app/lib/view_prefs.dart`、`app/lib/widgets/keyboard_mode_button.dart`。

## 验证

- 自动化通过：`chast-protocol-test` 覆盖 Legacy 往返与零 VK/越界 VK/非法动作/非按键字段拒绝；`chast-keyboard-test` 覆盖虚拟键注入参数（含扩展键）、按住/释放、重复过滤、修饰键与锁定键同步、锁定键敲击不误释放编号相同的 Legacy 按住键、切回 Map 后行为恢复、dry-run 注入序列不出现扫描码；Flutter `view_prefs_test` 覆盖 `legacy` 持久化与未知值回退，`flutter analyze` 无问题，完整 `flutter test` 通过。
- 已构建：主机库与测试、`chast-viewer-probe`、Windows 查看器 Release bundle；Release 全量 ctest **69/69** 通过（914.66 秒，无并发负载）。
- 待人工验证：真实双机非 US 布局、AltGr、死键、远端输入法、快捷键混合（Ctrl/Alt/Win 组合）、远程桌面嵌套和安全桌面输入；当前自动化只证明协议与注入参数，不证明目标应用的最终字符。
- 边界：虚拟键码是 Windows 概念，非 Windows 查看端不能产生该模式；本模式不替代本地输入法的 Unicode 提交，也不改默认的物理按键行为。
