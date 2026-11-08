import 'package:flutter/material.dart';
import '../l10n.dart';
import '../viewer_bridge.dart';

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
    icon: Icon(
      mode == ViewerKeyboardMode.local
          ? Icons.translate
          : Icons.keyboard_outlined,
    ),
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
            title: Text(
              value == ViewerKeyboardMode.physical ? tr('物理按键') : tr('本地输入法'),
            ),
            subtitle: Text(
              value == ViewerKeyboardMode.physical
                  ? tr('按键位置交给远端布局和输入法处理')
                  : tr('使用本机布局与输入法，确认后发送文字'),
            ),
          ),
        ),
    ],
  );
}
