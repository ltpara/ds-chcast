import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../l10n.dart';
import '../remote_shortcuts.dart';

class RemoteShortcutDialog extends StatefulWidget {
  const RemoteShortcutDialog({super.key, required this.initialBindings});

  final List<RemoteShortcutBinding> initialBindings;

  @override
  State<RemoteShortcutDialog> createState() => _RemoteShortcutDialogState();
}

class _RemoteShortcutDialogState extends State<RemoteShortcutDialog> {
  final FocusNode _captureFocus = FocusNode();
  late final Map<RemoteShortcutAction, RemoteShortcutBinding> _bindings =
      <RemoteShortcutAction, RemoteShortcutBinding>{
        for (final RemoteShortcutBinding binding in widget.initialBindings)
          binding.action: binding,
      };
  RemoteShortcutAction? _recording;
  String? _error;

  @override
  void dispose() {
    _captureFocus.dispose();
    super.dispose();
  }

  void _beginRecording(RemoteShortcutAction action) {
    setState(() {
      _recording = action;
      _error = null;
    });
    _captureFocus.requestFocus();
  }

  KeyEventResult _captureKey(FocusNode node, KeyEvent event) {
    if (_recording == null) return KeyEventResult.ignored;
    if (event is! KeyDownEvent) return KeyEventResult.handled;
    if (event.logicalKey == LogicalKeyboardKey.escape) {
      setState(() {
        _recording = null;
        _error = null;
      });
      return KeyEventResult.handled;
    }

    final int usage = remoteShortcutUsage(event);
    if (usage == 0) return KeyEventResult.handled;
    final int modifiers = remoteShortcutModifiers(HardwareKeyboard.instance);
    if (modifiers == 0) {
      setState(() => _error = tr('快捷键至少需要 Ctrl、Alt 或 Shift'));
      return KeyEventResult.handled;
    }
    if (RemoteShortcutBinding.isReservedRemoteShortcut(usage, modifiers)) {
      setState(() => _error = tr('此组合保留给系统或相对鼠标退出'));
      return KeyEventResult.handled;
    }

    final RemoteShortcutAction action = _recording!;
    final bool conflict = _bindings.values.any(
      (RemoteShortcutBinding binding) =>
          binding.action != action &&
          binding.keyUsage == usage &&
          binding.modifiers == modifiers,
    );
    if (conflict) {
      setState(() => _error = tr('此快捷键已分配给其他操作'));
      return KeyEventResult.handled;
    }
    final String label = event.logicalKey.keyLabel.trim();
    setState(() {
      _bindings[action] = RemoteShortcutBinding(
        action: action,
        keyUsage: usage,
        modifiers: modifiers,
        keyLabel: label.isEmpty ? '0x${usage.toRadixString(16)}' : label,
      );
      _recording = null;
      _error = null;
    });
    return KeyEventResult.handled;
  }

  @override
  Widget build(BuildContext context) => Focus(
    focusNode: _captureFocus,
    onKeyEvent: _captureKey,
    child: AlertDialog(
      title: Text(tr('会话快捷键')),
      content: SizedBox(
        width: 440,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            for (final RemoteShortcutAction action
                in RemoteShortcutAction.values)
              _buildRow(action),
            if (_recording != null)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Text(tr('按下 Ctrl、Alt 或 Shift 加一个按键；Esc 取消录制')),
              ),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    _error!,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.error,
                    ),
                  ),
                ),
              ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          onPressed: () => Navigator.of(
            context,
          ).pop(List<RemoteShortcutBinding>.unmodifiable(_bindings.values)),
          child: Text(tr('应用')),
        ),
      ],
    ),
  );

  Widget _buildRow(RemoteShortcutAction action) {
    final RemoteShortcutBinding? binding = _bindings[action];
    final bool recording = _recording == action;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(
        children: <Widget>[
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(tr(action.label)),
                Text(
                  recording ? tr('正在录制…') : binding?.chordLabel ?? tr('未设置'),
                  style: Theme.of(context).textTheme.bodySmall,
                ),
              ],
            ),
          ),
          IconButton(
            tooltip: recording ? tr('取消录制') : tr('录制快捷键'),
            onPressed: () => recording
                ? setState(() {
                    _recording = null;
                    _error = null;
                  })
                : _beginRecording(action),
            icon: Icon(recording ? Icons.stop_rounded : Icons.keyboard_rounded),
          ),
          IconButton(
            tooltip: tr('清除快捷键'),
            onPressed: binding == null
                ? null
                : () => setState(() {
                    _bindings.remove(action);
                    _error = null;
                  }),
            icon: const Icon(Icons.backspace_outlined),
          ),
        ],
      ),
    );
  }
}
