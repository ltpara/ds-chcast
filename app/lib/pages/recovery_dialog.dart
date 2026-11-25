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

typedef EmailVerificationRequester =
    Future<void> Function(ServerSettings settings, String token);

typedef EmailVerificationConfirmer =
    Future<void> Function(
      ServerSettings settings,
      String token, {
      required String verificationToken,
    });

typedef AlarmEmailPreferenceSetter =
    Future<void> Function(
      ServerSettings settings,
      String token, {
      required bool enabled,
    });

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
    this.requestEmailVerification,
    this.confirmEmailVerification,
    this.setAlarmEmailPreference,
  });

  final ServerSettings server;
  final AccountSession account;
  final RecoveryEmailChanger? changeEmail;
  final AccountInfoFetcher? infoFetcher;
  final EmailVerificationRequester? requestEmailVerification;
  final EmailVerificationConfirmer? confirmEmailVerification;
  final AlarmEmailPreferenceSetter? setAlarmEmailPreference;

  @override
  State<RecoveryEmailDialog> createState() => _RecoveryEmailDialogState();
}

class _RecoveryEmailDialogState extends State<RecoveryEmailDialog> {
  final TextEditingController _email = TextEditingController();
  final TextEditingController _password = TextEditingController();
  final TextEditingController _verificationToken = TextEditingController();
  bool _loading = true;
  bool _busy = false;
  bool _emailVerified = false;
  bool _alarmEmailEnabled = false;
  bool _showAlarmEmailPreference = false;
  bool _verificationSent = false;
  bool _didSave = false;
  String _storedEmail = '';
  String _loadError = '';
  String _error = '';
  String _status = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _email.dispose();
    _password.dispose();
    _verificationToken.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    try {
      final AccountInfo info =
          await (widget.infoFetcher ?? ServerApi.accountInfo)(
            widget.server,
            widget.account.token,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _loading = false;
        _storedEmail = info.email;
        _email.text = info.email;
        _emailVerified = info.emailVerified;
        _alarmEmailEnabled = info.alarmEmailEnabled;
        _showAlarmEmailPreference =
            info.alarmEmailAvailable && info.canReceiveSecurityAlerts;
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadError = tr('无法读取当前邮箱：{p1}', <String, Object?>{
            'p1': error.message,
          });
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadError = tr('无法读取当前邮箱：{error}', <String, Object?>{
            'error': error,
          });
        });
      }
    }
  }

  Future<void> _submit() async {
    if (_busy) {
      return;
    }
    final String email = _email.text.trim();
    if (email == _storedEmail) {
      setState(() => _error = tr('邮箱没有变化'));
      return;
    }
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
      final String stored =
          await (widget.changeEmail ?? ServerApi.changeRecoveryEmail)(
            widget.server,
            widget.account.token,
            email: email,
            password: _password.text,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _storedEmail = stored;
        _email.text = stored;
        _emailVerified = false;
        _alarmEmailEnabled = false;
        _verificationSent = false;
        _verificationToken.clear();
        _password.clear();
        _didSave = true;
        _error = '';
        _status = stored.isEmpty ? tr('找回邮箱已清除') : tr('邮箱已保存，请发送验证邮件确认所有权');
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
          _error = tr('保存失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _sendVerification() async {
    if (_busy || _storedEmail.isEmpty || _email.text.trim() != _storedEmail) {
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _status = '';
    });
    try {
      await (widget.requestEmailVerification ??
          ServerApi.requestEmailVerification)(
        widget.server,
        widget.account.token,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _verificationSent = true;
        _verificationToken.clear();
        _status = tr('验证邮件已发送，请输入邮件中的令牌');
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
          _error = tr('发送验证邮件失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _confirmVerification() async {
    if (_busy) {
      return;
    }
    final String token = _verificationToken.text.trim();
    if (token.isEmpty) {
      setState(() => _error = tr('请输入邮件中的邮箱验证令牌'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _status = '';
    });
    try {
      await (widget.confirmEmailVerification ??
          ServerApi.confirmEmailVerification)(
        widget.server,
        widget.account.token,
        verificationToken: token,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _emailVerified = true;
        _verificationSent = false;
        _verificationToken.clear();
        _status = tr('邮箱已验证');
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
          _error = tr('验证邮箱失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _setAlarmEmailPreference(bool enabled) async {
    if (_busy || !_showAlarmEmailPreference) {
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await (widget.setAlarmEmailPreference ??
          ServerApi.setAlarmEmailPreference)(
        widget.server,
        widget.account.token,
        enabled: enabled,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _alarmEmailEnabled = enabled;
        _status = enabled ? tr('安全告警邮件已开启') : tr('安全告警邮件已关闭');
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
          _error = tr('更新安全告警邮件偏好失败：{error}', <String, Object?>{
            'error': error,
          });
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool emailMatchesStored = _email.text.trim() == _storedEmail;
    final bool canSendVerification =
        _storedEmail.isNotEmpty && !_emailVerified && emailMatchesStored;
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(tr('找回邮箱')),
      content: SizedBox(
        width: 380,
        child: SingleChildScrollView(
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
                  style: TextStyle(
                    color: theme.colorScheme.error,
                    fontSize: 11,
                  ),
                )
              else
                Text(
                  _storedEmail.isEmpty
                      ? tr('当前邮箱：未设置')
                      : tr('当前邮箱：{p2}', <String, Object?>{'p2': _storedEmail}),
                  key: const Key('recovery-email-current'),
                  style: mutedStyle(context, size: 11),
                ),
              if (_storedEmail.isNotEmpty) ...<Widget>[
                const SizedBox(height: 4),
                Text(
                  _emailVerified ? tr('邮箱已验证') : tr('邮箱尚未验证'),
                  key: const Key('recovery-email-verification-state'),
                  style: mutedStyle(context, size: 11),
                ),
              ],
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
                onChanged: (_) {
                  setState(() {
                    _error = '';
                    _status = '';
                    if (_email.text.trim() != _storedEmail) {
                      _verificationSent = false;
                      _verificationToken.clear();
                    }
                  });
                },
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
                  style: TextStyle(
                    color: theme.colorScheme.error,
                    fontSize: 12,
                  ),
                ),
              ],
              if (_status.isNotEmpty) ...<Widget>[
                const SizedBox(height: 10),
                Text(
                  _status,
                  key: const Key('recovery-email-status'),
                  style: mutedStyle(context, size: 12),
                ),
              ],
              if (_storedEmail.isNotEmpty && !_emailVerified) ...<Widget>[
                const SizedBox(height: 12),
                OutlinedButton.icon(
                  key: const Key('recovery-email-send-verification'),
                  onPressed: _busy || !canSendVerification
                      ? null
                      : _sendVerification,
                  icon: const Icon(Icons.mark_email_unread_outlined, size: 17),
                  label: Text(tr('发送验证邮件')),
                ),
                if (_verificationSent && emailMatchesStored) ...<Widget>[
                  TextField(
                    key: const Key('recovery-email-verification-token'),
                    controller: _verificationToken,
                    autocorrect: false,
                    enableSuggestions: false,
                    decoration: InputDecoration(
                      labelText: tr('邮箱验证令牌'),
                      prefixIcon: Icon(Icons.verified_user_outlined, size: 18),
                    ),
                    onSubmitted: (_) => _confirmVerification(),
                  ),
                  const SizedBox(height: 8),
                  FilledButton.tonal(
                    key: const Key('recovery-email-confirm-verification'),
                    onPressed: _busy ? null : _confirmVerification,
                    child: Text(tr('确认邮箱')),
                  ),
                ],
              ],
              if (_showAlarmEmailPreference) ...<Widget>[
                const Divider(height: 24),
                SwitchListTile.adaptive(
                  key: const Key('recovery-email-alarm-preference'),
                  contentPadding: EdgeInsets.zero,
                  title: Text(tr('通过邮件接收安全告警')),
                  subtitle: Text(
                    _emailVerified
                        ? tr('仅发送与当前全局审计权限相关的安全事件')
                        : tr('验证找回邮箱后即可开启'),
                  ),
                  value: _alarmEmailEnabled,
                  onChanged: _busy || !_emailVerified
                      ? null
                      : _setAlarmEmailPreference,
                ),
              ],
            ],
          ),
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('recovery-email-close'),
          onPressed: _busy
              ? null
              : () => Navigator.of(context).pop(_didSave ? _storedEmail : null),
          child: Text(_didSave ? tr('完成') : tr('取消')),
        ),
        FilledButton(
          key: const Key('recovery-email-save'),
          onPressed: _busy || _loading || emailMatchesStored ? null : _submit,
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