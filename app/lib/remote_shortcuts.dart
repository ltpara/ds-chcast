import 'package:flutter/services.dart';

enum RemoteShortcutAction {
  toggleControl('toggleControl', '切换远程控制'),
  toggleFullscreen('toggleFullscreen', '切换全屏'),
  toggleRelativeMouse('toggleRelativeMouse', '切换相对鼠标'),
  toggleStats('toggleStats', '切换统计面板'),
  leaveSession('leaveSession', '结束会话');

  const RemoteShortcutAction(this.id, this.label);

  final String id;
  final String label;

  static RemoteShortcutAction? fromId(Object? value) {
    for (final RemoteShortcutAction action in values) {
      if (action.id == value) return action;
    }
    return null;
  }
}

class RemoteShortcutBinding {
  const RemoteShortcutBinding({
    required this.action,
    required this.keyUsage,
    required this.modifiers,
    required this.keyLabel,
  });

  final RemoteShortcutAction action;
  final int keyUsage;

  /// Ctrl=1, Alt=2, Shift=4. Windows-key bindings are intentionally excluded
  /// because Windows reserves many of them before the viewer can handle them.
  final int modifiers;
  final String keyLabel;

  String get chordLabel => <String>[
    if (modifiers & 1 != 0) 'Ctrl',
    if (modifiers & 2 != 0) 'Alt',
    if (modifiers & 4 != 0) 'Shift',
    keyLabel,
  ].join('+');

  bool matches(int usage, int activeModifiers) =>
      keyUsage == usage && modifiers == activeModifiers;

  @override
  bool operator ==(Object other) =>
      other is RemoteShortcutBinding &&
      action == other.action &&
      keyUsage == other.keyUsage &&
      modifiers == other.modifiers &&
      keyLabel == other.keyLabel;

  @override
  int get hashCode => Object.hash(action, keyUsage, modifiers, keyLabel);

  Map<String, Object> toStorage() => <String, Object>{
    'action': action.id,
    'keyUsage': keyUsage,
    'modifiers': modifiers,
    'keyLabel': keyLabel,
  };

  Map<String, Object> toNative() => <String, Object>{
    'action': action.id,
    'keyUsage': keyUsage,
    'modifiers': modifiers,
  };

  static List<RemoteShortcutBinding> parseList(Object? value) {
    if (value is! Iterable) return const <RemoteShortcutBinding>[];
    final List<RemoteShortcutBinding> result = <RemoteShortcutBinding>[];
    final Set<String> actions = <String>{};
    final Set<String> chords = <String>{};
    for (final Object? raw in value) {
      if (raw is! Map) continue;
      final RemoteShortcutAction? action = RemoteShortcutAction.fromId(
        raw['action'],
      );
      final Object? usageValue = raw['keyUsage'];
      final Object? modifiersValue = raw['modifiers'];
      final Object? labelValue = raw['keyLabel'];
      if (action == null ||
          usageValue is! int ||
          modifiersValue is! int ||
          !isAllowedRemoteShortcutKey(usageValue) ||
          modifiersValue < 1 ||
          modifiersValue > 7 ||
          labelValue is! String ||
          labelValue.isEmpty ||
          labelValue.length > 16 ||
          isReservedRemoteShortcut(usageValue, modifiersValue)) {
        continue;
      }
      final String chord = '$usageValue:$modifiersValue';
      if (actions.contains(action.id) || chords.contains(chord)) continue;
      actions.add(action.id);
      chords.add(chord);
      result.add(
        RemoteShortcutBinding(
          action: action,
          keyUsage: usageValue,
          modifiers: modifiersValue,
          keyLabel: labelValue,
        ),
      );
      if (result.length == RemoteShortcutAction.values.length) break;
    }
    return List<RemoteShortcutBinding>.unmodifiable(result);
  }

  static bool isAllowedRemoteShortcutKey(int usage) =>
      usage >= 0x04 &&
      usage < 0xe0 &&
      usage != 0x29 && // Escape cancels shortcut recording.
      usage != 0x39 &&
      usage != 0x47 &&
      usage != 0x53;

  static bool isReservedRemoteShortcut(int usage, int modifiers) =>
      (usage == 0x2b && modifiers & 2 != 0) || // Alt+Tab
      (usage == 0x3d && modifiers & 2 != 0) || // Alt+F4
      (usage == 0x29 && modifiers == 5) || // Ctrl+Shift+Esc / Task Manager
      (usage == 0x2b &&
          modifiers & 3 == 3) || // Ctrl+Alt+Tab / Windows switcher
      (usage == 0x2c) || // Alt/Ctrl/Shift+Space (window/layout/IME)
      (usage == 0x4c && modifiers == 3) ||
      (usage == 0x15 && modifiers == 7);
}

int remoteShortcutUsage(KeyEvent event) {
  final int raw = event.physicalKey.usbHidUsage;
  if (raw >> 16 != 0x07) return 0;
  final int usage = raw & 0xffff;
  return RemoteShortcutBinding.isAllowedRemoteShortcutKey(usage) ? usage : 0;
}

int remoteShortcutModifiers(HardwareKeyboard keyboard) {
  if (keyboard.isMetaPressed ||
      keyboard.physicalKeysPressed.contains(PhysicalKeyboardKey.altRight)) {
    // Right Alt is also AltGr on many layouts. Do not steal IME/text input.
    return 0;
  }
  return (keyboard.isControlPressed ? 1 : 0) |
      (keyboard.isAltPressed ? 2 : 0) |
      (keyboard.isShiftPressed ? 4 : 0);
}
