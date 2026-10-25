import 'dart:convert';

import 'package:flutter/material.dart';

import '../account_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';

typedef PasswordChanger = Future<void> Function(
  ServerSettings settings,
  String token, {
  required String currentPassword,
  required String newPassword,
});

/// Changes the signed-in account's password. The server keeps this device
/// signed in and revokes every other session, so the dialog explains the
/// consequence before submitting.
class ChangePasswordDialog extends StatefulWidget {
  const ChangePasswordDialog({
    super.key,
    required this.server,
    required this.account,
    this.changePassword,
  });

  final ServerSettings server;
  final AccountSession account;
  final PasswordChanger? changePassword;

  @override
  State<ChangePasswordDialog> createState() => _ChangePasswordDialogState();
}

class _ChangePasswordDialogState extends State<ChangePasswordDialog> {
  final TextEditingController _current = TextEditingController();
  final TextEditingController _next = TextEditingController();
  final TextEditingController _confirm = TextEditingController();
  bool _busy = false;
  String _error = '';

  @override
  void dispose() {
    _current.dispose();
    _next.dispose();
    _confirm.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    if (_busy) {
      return;
    }
    final String current = _current.text;
    final String next = _next.text;
    final int bytes = utf8.encode(next).length;
    if (current.isEmpty) {
      setState(() => _error = '请输入当前密码');
      return;
    }
    if (bytes < 8 || bytes > 128) {
      setState(() => _error = '新密码长度需为 8–128 个字节');
      return;
    }
    if (next != _confirm.text) {
      setState(() => _error = '两次输入的新密码不一致');
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await (widget.changePassword ?? ServerApi.changePassword)(
        widget.server,
        widget.account.token,
        currentPassword: current,
        newPassword: next,
      );
      if (!mounted) {
        return;
      }
      Navigator.of(context).pop(true);
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = '修改密码失败：$error';
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: const Text('修改密码'),
      content: SizedBox(
        width: 360,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              '修改后其他设备需要使用新密码重新登录，本机保持登录。',
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 14),
            TextField(
              key: const Key('change-password-current'),
              controller: _current,
              autofocus: true,
              obscureText: true,
              autocorrect: false,
              enableSuggestions: false,
              decoration: const InputDecoration(
                labelText: '当前密码',
                prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
              ),
            ),
            const SizedBox(height: 10),
            TextField(
              key: const Key('change-password-new'),
              controller: _next,
              obscureText: true,
              autocorrect: false,
              enableSuggestions: false,
              decoration: const InputDecoration(
                labelText: '新密码（8–128 字节）',
                prefixIcon: Icon(Icons.password_rounded, size: 18),
              ),
            ),
            const SizedBox(height: 10),
            TextField(
              key: const Key('change-password-confirm'),
              controller: _confirm,
              obscureText: true,
              autocorrect: false,
              enableSuggestions: false,
              decoration: const InputDecoration(
                labelText: '确认新密码',
                prefixIcon: Icon(Icons.check_circle_outline_rounded, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            ),
            if (_error.isNotEmpty) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _error,
                key: const Key('change-password-error'),
                style: TextStyle(
                  color: theme.colorScheme.error,
                  fontSize: 12,
                ),
              ),
            ],
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
        FilledButton(
          key: const Key('change-password-submit'),
          onPressed: _busy ? null : _submit,
          child: _busy
              ? const SizedBox(
                  width: 15,
                  height: 15,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : const Text('修改密码'),
        ),
      ],
    );
  }
}
