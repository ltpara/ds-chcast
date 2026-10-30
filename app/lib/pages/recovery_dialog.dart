import 'dart:convert';

import 'package:flutter/material.dart';

import '../account_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';
import 'package:chast_viewer/l10n.dart';

typedef RecoveryEmailChanger = Future<String> Function(
  ServerSettings settings,
  String token, {
  required String email,
  required String password,
});

typedef RecoveryRequester = Future<void> Function(
  ServerSettings settings, {
  required String username,
});

typedef RecoveryCompleter = Future<String> Function(
  ServerSettings settings, {
  required String token,
  required String password,
});

typedef AccountInfoFetcher = Future<AccountInfo> Function(
  ServerSettings settings,
  String token,
);

typedef RecoveryPhoneChanger = Future<String> Function(
  ServerSettings settings,
  String token, {
  required String phone,
  required String password,
});

typedef SMSRecoveryRequester = Future<void> Function(
  ServerSettings settings, {
  required String username,
});

typedef SMSRecoveryCompleter = Future<String> Function(
  ServerSettings settings, {
  required String username,
  required String code,
  required String password,
});

/// Sets or clears the address that receives password-reset mail. The current
/// password is required so a borrowed token cannot point resets elsewhere.
class RecoveryEmailDialog extends StatefulWidget {
  const RecoveryEmailDialog({
    super.key,
    required this.server,
    required this.account,
    this.changeEmail,
    this.infoFetcher,
  });

  final ServerSettings server;
  final AccountSession account;
  final RecoveryEmailChanger? changeEmail;
  final AccountInfoFetcher? infoFetcher;

  @override
  State<RecoveryEmailDialog> createState() => _RecoveryEmailDialogState();
}

class _RecoveryEmailDialogState extends State<RecoveryEmailDialog> {
  final TextEditingController _email = TextEditingController();
  final TextEditingController _password = TextEditingController();
  bool _loading = true;
  bool _busy = false;
  String _loadError = '';
  String _error = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _email.dispose();
    _password.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    try {
      final AccountInfo info = await (widget.infoFetcher ??
          ServerApi.accountInfo)(widget.server, widget.account.token);
      if (!mounted) {
        return;
      }
      setState(() {
        _loading = false;
        if (info.email.isNotEmpty) {
          _email.text = info.email;
        }
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadError = tr('无法读取当前邮箱：{p1}', <String, Object?>{'p1': error.message});
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadError = tr('无法读取当前邮箱：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _submit() async {
    if (_busy) {
      return;
    }
    final String email = _email.text.trim();
    if (email.isNotEmpty && !email.contains('@')) {
      setState(() => _error = tr('请输入有效的邮箱地址'));
      return;
    }
    if (_password.text.isEmpty) {
      setState(() => _error = tr('请输入当前密码'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      final String stored = await (widget.changeEmail ??
          ServerApi.changeRecoveryEmail)(
        widget.server,
        widget.account.token,
        email: email,
        password: _password.text,
      );
      if (!mounted) {
        return;
      }
      Navigator.of(context).pop(stored);
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
          _error = tr('保存失败：{error}', <String, Object?>{'error': error});
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
      title: Text(tr('找回邮箱')),
      content: SizedBox(
        width: 380,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              tr('忘记密码时，服务端会把一次性重置令牌发送到该邮箱。留空并保存表示关闭邮件找回。'),
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 12),
            if (_loading)
              Text(tr('正在读取当前邮箱…'), style: mutedStyle(context, size: 11))
            else if (_loadError.isNotEmpty)
              Text(
                _loadError,
                key: const Key('recovery-email-load-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 11),
              )
            else
              Text(
                _email.text.isEmpty ? tr('当前邮箱：未设置') : tr('当前邮箱：{p2}', <String, Object?>{'p2': _email.text}),
                key: const Key('recovery-email-current'),
                style: mutedStyle(context, size: 11),
              ),
            const SizedBox(height: 12),
            TextField(
              key: const Key('recovery-email-value'),
              controller: _email,
              keyboardType: TextInputType.emailAddress,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('找回邮箱'),
                hintText: 'name@example.com',
                prefixIcon: Icon(Icons.alternate_email_rounded, size: 18),
              ),
            ),
            const SizedBox(height: 10),
            TextField(
              key: const Key('recovery-email-password'),
              controller: _password,
              obscureText: true,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('当前密码'),
                prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            ),
            if (_error.isNotEmpty) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _error,
                key: const Key('recovery-email-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 12),
              ),
            ],
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('recovery-email-save'),
          onPressed: _busy || _loading ? null : _submit,
          child: _busy
              ? const SizedBox(
                  width: 15,
                  height: 15,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : Text(tr('保存')),
        ),
      ],
    );
  }
}

/// Sets or clears the number that receives SMS reset codes. Like the email
/// dialog it requires the current password before the change.
class RecoveryPhoneDialog extends StatefulWidget {
  const RecoveryPhoneDialog({
    super.key,
    required this.server,
    required this.account,
    this.changePhone,
    this.infoFetcher,
  });

  final ServerSettings server;
  final AccountSession account;
  final RecoveryPhoneChanger? changePhone;
  final AccountInfoFetcher? infoFetcher;

  @override
  State<RecoveryPhoneDialog> createState() => _RecoveryPhoneDialogState();
}

class _RecoveryPhoneDialogState extends State<RecoveryPhoneDialog> {
  final TextEditingController _phone = TextEditingController();
  final TextEditingController _password = TextEditingController();
  bool _loading = true;
  bool _busy = false;
  String _loadError = '';
  String _error = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _phone.dispose();
    _password.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    try {
      final AccountInfo info = await (widget.infoFetcher ??
          ServerApi.accountInfo)(widget.server, widget.account.token);
      if (!mounted) {
        return;
      }
      setState(() {
        _loading = false;
        if (info.phone.isNotEmpty) {
          _phone.text = info.phone;
        }
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadError = tr('无法读取当前手机号：{p3}', <String, Object?>{'p3': error.message});
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadError = tr('无法读取当前手机号：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _submit() async {
    if (_busy) {
      return;
    }
    final String phone = _phone.text.trim();
    if (!_validPhoneInput(phone)) {
      setState(() => _error = tr('手机号需为 6–15 位数字'));
      return;
    }
    if (_password.text.isEmpty) {
      setState(() => _error = tr('请输入当前密码'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      final String stored = await (widget.changePhone ??
          ServerApi.changeRecoveryPhone)(
        widget.server,
        widget.account.token,
        phone: phone,
        password: _password.text,
      );
      if (!mounted) {
        return;
      }
      Navigator.of(context).pop(stored);
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
          _error = tr('保存失败：{error}', <String, Object?>{'error': error});
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
      title: Text(tr('找回手机')),
      content: SizedBox(
        width: 380,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              tr('忘记密码时，服务端会把一次性验证码发送到该手机号。留空并保存表示关闭短信找回。'),
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 12),
            if (_loading)
              Text(tr('正在读取当前手机号…'), style: mutedStyle(context, size: 11))
            else if (_loadError.isNotEmpty)
              Text(
                _loadError,
                key: const Key('recovery-phone-load-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 11),
              )
            else
              Text(
                _phone.text.isEmpty ? tr('当前手机号：未设置') : tr('当前手机号：{p4}', <String, Object?>{'p4': _phone.text}),
                key: const Key('recovery-phone-current'),
                style: mutedStyle(context, size: 11),
              ),
            const SizedBox(height: 12),
            TextField(
              key: const Key('recovery-phone-value'),
              controller: _phone,
              keyboardType: TextInputType.phone,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('找回手机'),
                hintText: '+1 555 010 0001',
                prefixIcon: Icon(Icons.smartphone_rounded, size: 18),
              ),
            ),
            const SizedBox(height: 10),
            TextField(
              key: const Key('recovery-phone-password'),
              controller: _password,
              obscureText: true,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('当前密码'),
                prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            ),
            if (_error.isNotEmpty) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _error,
                key: const Key('recovery-phone-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 12),
              ),
            ],
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('recovery-phone-save'),
          onPressed: _busy || _loading ? null : _submit,
          child: _busy
              ? const SizedBox(
                  width: 15,
                  height: 15,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : Text(tr('保存')),
        ),
      ],
    );
  }
}

/// Two-step password recovery for a signed-out viewer: request a mail or SMS,
/// then enter its token/code together with a new password. The server answers
/// every request the same way, so this dialog also avoids revealing whether an
/// account exists.
class ForgotPasswordDialog extends StatefulWidget {
  const ForgotPasswordDialog({
    super.key,
    required this.server,
    this.initialUsername = '',
    this.requestRecovery,
    this.completeRecovery,
    this.requestSMSRecovery,
    this.completeSMSRecovery,
  });

  final ServerSettings server;
  final String initialUsername;
  final RecoveryRequester? requestRecovery;
  final RecoveryCompleter? completeRecovery;
  final SMSRecoveryRequester? requestSMSRecovery;
  final SMSRecoveryCompleter? completeSMSRecovery;

  @override
  State<ForgotPasswordDialog> createState() => _ForgotPasswordDialogState();
}

class _ForgotPasswordDialogState extends State<ForgotPasswordDialog> {
  late final TextEditingController _username =
      TextEditingController(text: widget.initialUsername);
  final TextEditingController _token = TextEditingController();
  final TextEditingController _password = TextEditingController();
  final TextEditingController _confirm = TextEditingController();
  bool _sent = false;
  bool _useSMS = false;
  bool _busy = false;
  String _notice = '';
  String _error = '';

  @override
  void dispose() {
    _username.dispose();
    _token.dispose();
    _password.dispose();
    _confirm.dispose();
    super.dispose();
  }

  Future<void> _send() async {
    if (_busy) {
      return;
    }
    final String username = _username.text.trim();
    if (username.isEmpty) {
      setState(() => _error = tr('请输入用户名'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      final RecoveryRequester request = _useSMS
          ? widget.requestSMSRecovery ?? ServerApi.requestSMSRecovery
          : widget.requestRecovery ?? ServerApi.requestPasswordRecovery;
      await request(widget.server, username: username);
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _sent = true;
        _notice = _useSMS
            ? tr('如果该账号存在且已设置找回手机，6–15 位号码会收到 8 位验证码。请查看短信。')
            : tr('如果该账号存在且已设置找回邮箱，重置令牌已发送。请检查邮件（含垃圾箱）。');
      });
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
          _error = tr('发送失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _complete() async {
    if (_busy) {
      return;
    }
    final String token = _token.text.trim();
    final int bytes = utf8.encode(_password.text).length;
    if (token.isEmpty) {
      setState(() => _error = _useSMS ? tr('请输入短信中的验证码') : tr('请输入邮件中的重置令牌'));
      return;
    }
    if (_useSMS && _smsDigits(token).length != 8) {
      setState(() => _error = tr('短信验证码需为 8 位数字'));
      return;
    }
    if (bytes < 8 || bytes > 128) {
      setState(() => _error = tr('新密码长度需为 8–128 个字节'));
      return;
    }
    if (_password.text != _confirm.text) {
      setState(() => _error = tr('两次输入的新密码不一致'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      final String username = _useSMS
          ? await (widget.completeSMSRecovery ?? ServerApi.completeSMSRecovery)(
              widget.server,
              username: _username.text.trim(),
              code: token,
              password: _password.text,
            )
          : await (widget.completeRecovery ??
              ServerApi.completePasswordRecovery)(
              widget.server,
              token: token,
              password: _password.text,
            );
      if (!mounted) {
        return;
      }
      Navigator.of(context).pop(username);
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
          _error = tr('重置失败：{error}', <String, Object?>{'error': error});
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
      title: Text(tr('找回密码')),
      content: SizedBox(
        width: 380,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              _sent
                  ? (_useSMS
                      ? tr('把手机短信里的 8 位验证码粘贴到下方，并设置新密码。重置成功后，该账号在所有设备上的登录都会失效。')
                      : tr('把邮件里的令牌粘贴到下方，并设置新密码。重置成功后，该账号在所有设备上的登录都会失效。'))
                  : (_useSMS
                      ? tr('输入账号用户名，服务端会向该账号的找回手机发送 8 位验证码。没有设置手机号的账号不会收到短信。')
                      : tr('输入账号用户名，服务端会向该账号的找回邮箱发送一次性令牌。没有设置邮箱的账号不会收到邮件。')),
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 12),
            if (!_sent) ...<Widget>[
              Wrap(
                spacing: 8,
                children: <Widget>[
                  ChoiceChip(
                    key: const Key('forgot-channel-email'),
                    label: Text(tr('邮箱')),
                    avatar: const Icon(Icons.alternate_email_rounded, size: 16),
                    selected: !_useSMS,
                    onSelected: _busy
                        ? null
                        : (_) => setState(() {
                              _useSMS = false;
                              _error = '';
                            }),
                  ),
                  ChoiceChip(
                    key: const Key('forgot-channel-sms'),
                    label: Text(tr('短信')),
                    avatar: const Icon(Icons.sms_outlined, size: 16),
                    selected: _useSMS,
                    onSelected: _busy
                        ? null
                        : (_) => setState(() {
                              _useSMS = true;
                              _error = '';
                            }),
                  ),
                ],
              ),
              const SizedBox(height: 12),
              TextField(
                key: const Key('forgot-username'),
                controller: _username,
                autofocus: true,
                autocorrect: false,
                enableSuggestions: false,
                decoration: InputDecoration(
                  labelText: tr('用户名'),
                  prefixIcon: Icon(Icons.person_outline_rounded, size: 18),
                ),
                onSubmitted: (_) => _send(),
              ),
            ] else ...<Widget>[
              Text(
                _notice,
                key: const Key('forgot-notice'),
                style: TextStyle(
                  color: theme.colorScheme.primary,
                  fontSize: 12,
                ),
              ),
              const SizedBox(height: 12),
              TextField(
                key: const Key('forgot-token'),
                controller: _token,
                autofocus: true,
                autocorrect: false,
                enableSuggestions: false,
                decoration: InputDecoration(
                  labelText: _useSMS ? tr('短信验证码') : tr('重置令牌'),
                  hintText: _useSMS ? tr('8 位数字') : null,
                  prefixIcon: Icon(
                    _useSMS ? Icons.sms_outlined : Icons.key_outlined,
                    size: 18,
                  ),
                ),
              ),
              const SizedBox(height: 10),
              TextField(
                key: const Key('forgot-password'),
                controller: _password,
                obscureText: true,
                autocorrect: false,
                enableSuggestions: false,
                decoration: InputDecoration(
                  labelText: tr('新密码（8–128 字节）'),
                  prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
                ),
              ),
              const SizedBox(height: 10),
              TextField(
                key: const Key('forgot-confirm'),
                controller: _confirm,
                obscureText: true,
                autocorrect: false,
                enableSuggestions: false,
                decoration: InputDecoration(
                  labelText: tr('确认新密码'),
                  prefixIcon: Icon(Icons.check_circle_outline_rounded, size: 18),
                ),
                onSubmitted: (_) => _complete(),
              ),
            ],
            if (_error.isNotEmpty) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _error,
                key: const Key('forgot-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 12),
              ),
            ],
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        if (_sent)
          TextButton(
            key: const Key('forgot-back'),
            onPressed: _busy
                ? null
                : () => setState(() {
                      _sent = false;
                      _notice = '';
                      _error = '';
                    }),
            child: Text(tr('换个账号')),
          ),
        FilledButton(
          key: Key(_sent ? 'forgot-complete' : 'forgot-send'),
          onPressed: _busy ? null : (_sent ? _complete : _send),
          child: _busy
              ? const SizedBox(
                  width: 15,
                  height: 15,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : Text(_sent ? tr('重置密码') : (_useSMS ? tr('发送验证码') : tr('发送重置令牌'))),
        ),
      ],
    );
  }
}

bool _validPhoneInput(String value) {
  if (value.isEmpty) {
    return true;
  }
  if (!RegExp(r'^\+?[0-9 ()\-\.]+$').hasMatch(value)) {
    return false;
  }
  final int digits = value.replaceAll(RegExp(r'[^0-9]'), '').length;
  return digits >= 6 && digits <= 15;
}

String _smsDigits(String value) => value.replaceAll(RegExp(r'[\s-]'), '');
