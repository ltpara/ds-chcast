import 'package:flutter/material.dart';
import '../l10n.dart';
import '../viewer_bridge.dart';

IconData _modeIcon(ViewerKeyboardMode mode) {
  switch (mode) {
    case ViewerKeyboardMode.physical:
      return Icons.keyboard_outlined;
    case ViewerKeyboardMode.legacy:
      return Icons.keyboard_alt_outlined;
    case ViewerKeyboardMode.local:
      return Icons.translate;
  }
}

String _modeTitle(ViewerKeyboardMode mode) {
  switch (mode) {
    case ViewerKeyboardMode.physical:
      return tr('物理按键');
    case ViewerKeyboardMode.legacy:
      return tr('传统按键');
    case ViewerKeyboardMode.local:
      return tr('本地输入法');
  }
}

String _modeSubtitle(ViewerKeyboardMode mode) {
  switch (mode) {
    case ViewerKeyboardMode.physical:
      return tr('按键位置交给远端布局和输入法处理');
    case ViewerKeyboardMode.legacy:
      return tr('发送虚拟键码，由远端布局和输入法解释');
    case ViewerKeyboardMode.local:
      return tr('使用本机布局与输入法，确认后发送文字');
  }
}

class KeyboardModeButton extends StatelessWidget {
  const KeyboardModeButton({super.key, required this.mode, this.onChanged});
  final ViewerKeyboardMode mode;
  final ValueChanged<ViewerKeyboardMode>? onChanged;

  @override
  Widget build(BuildContext context) => PopupMenuButton<ViewerKeyboardMode>(
    tooltip: tr('键盘模式'),
    enabled: onChanged != null,
    initialValue: mode,
    onSelected: onChanged,
    icon: Icon(_modeIcon(mode)),
    itemBuilder: (context) => [
      for (final value in ViewerKeyboardMode.values)
        PopupMenuItem(
          value: value,
          child: ListTile(
            contentPadding: EdgeInsets.zero,
            leading: Icon(
              mode == value
                  ? Icons.radio_button_checked
                  : Icons.radio_button_off,
            ),
            title: Text(_modeTitle(value)),
            subtitle: Text(_modeSubtitle(value)),
          ),
        ),
    ],
  );
}
