import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'package:chast_viewer/l10n.dart';

/// The protocol's per-block ceiling (kMaxTextInputBytes).
const int kMaxTextInputBytes = 4096;

/// Collects text composed on this machine and hands it to the host to type as
/// Unicode. The text never goes through the host's keyboard layout, so a
/// local input method and scripts the host layout lacks arrive intact.
class SendTextDialog extends StatefulWidget {
  const SendTextDialog({super.key, required this.onSend});

  /// Returns true when the block was queued for the streaming session.
  final Future<bool> Function(String text) onSend;

  /// Null when [text] can be sent, otherwise a message for the inline hint.
  /// Unlike chat, whitespace is meaningful here: a lone newline is a valid
  /// request to press Return on the host.
  static String? validate(String text) {
    if (text.isEmpty) {
      return tr('文本不能为空');
    }
    if (text.contains('\u0000')) {
      return tr('文本不能包含空字符');
    }
    final int bytes = utf8.encode(text).length;
    if (bytes > kMaxTextInputBytes) {
      return tr('文本过长：{bytes} / {kMaxTextInputBytes} 字节', <String, Object?>{
        'bytes': bytes,
        'kMaxTextInputBytes': kMaxTextInputBytes,
      });
    }
    return null;
  }

  @override
  State<SendTextDialog> createState() => _SendTextDialogState();
}

class _SendTextDialogState extends State<SendTextDialog> {
  final TextEditingController _text = TextEditingController();
  bool _sending = false;
  String? _error;

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    if (_sending) {
      return;
    }
    final String text = _text.text;
    final String? invalid = SendTextDialog.validate(text);
    if (invalid != null) {
      setState(() => _error = invalid);
      return;
    }
    setState(() {
      _sending = true;
      _error = null;
    });
    bool queued = false;
    try {
      queued = await widget.onSend(text);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    } finally {
      if (mounted) {
        setState(() => _sending = false);
      }
    }
    if (!mounted) {
      return;
    }
    if (!queued) {
      setState(() => _error = tr('发送失败：会话未就绪、未开启输入或发送队列已满'));
      return;
    }
    Navigator.of(context).pop(true);
  }

  @override
  Widget build(BuildContext context) {
    final int bytes = utf8.encode(_text.text).length;
    return AlertDialog(
      title: Text(tr('发送文本')),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              tr('文本会以 Unicode 发送，不依赖主机键盘布局'),
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: 10),
            TextField(
              controller: _text,
              autofocus: true,
              minLines: 4,
              maxLines: 8,
              onChanged: (_) => setState(() => _error = null),
              decoration: InputDecoration(
                hintText: tr('输入要在这台主机上键入的文本'),
                border: const OutlineInputBorder(),
                helperText: '$bytes / $kMaxTextInputBytes',
              ),
            ),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Text(
                  _error!,
                  style: TextStyle(color: Theme.of(context).colorScheme.error),
                ),
              ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _sending ? null : () => Navigator.of(context).pop(false),
          child: Text(tr('取消')),
        ),
        FilledButton(
          onPressed: _sending ? null : _submit,
          child: Text(tr('发送')),
        ),
      ],
    );
  }
}
