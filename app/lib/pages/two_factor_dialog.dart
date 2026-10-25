import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../account_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';

typedef TwoFactorStatusLoader = Future<AccountInfo> Function(
  ServerSettings settings,
  String token,
);
typedef TwoFactorStarter = Future<TwoFactorEnrollment> Function(
  ServerSettings settings,
  String token,
);
typedef TwoFactorEnabler = Future<void> Function(
  ServerSettings settings,
  String token,
  String code,
);
typedef TwoFactorDisabler = Future<void> Function(
  ServerSettings settings,
  String token, {
  required String password,
  required String code,
});
typedef TwoFactorRecoveryGenerator = Future<List<String>> Function(
  ServerSettings settings,
  String token, {
  required String password,
  required String code,
});

/// Enrolls or removes a TOTP second factor for the signed-in account. The
/// secret is shown for manual entry because the viewer does not ship a QR
/// scanner; disabling also requires the password and a current code.
class TwoFactorDialog extends StatefulWidget {
  const TwoFactorDialog({
    super.key,
    required this.server,
    required this.account,
    this.loadInfo,
    this.start,
    this.enable,
    this.disable,
    this.generateRecoveryCodes,
  });

  final ServerSettings server;
  final AccountSession account;
  final TwoFactorStatusLoader? loadInfo;
  final TwoFactorStarter? start;
  final TwoFactorEnabler? enable;
  final TwoFactorDisabler? disable;
  final TwoFactorRecoveryGenerator? generateRecoveryCodes;

  @override
  State<TwoFactorDialog> createState() => _TwoFactorDialogState();
}

class _TwoFactorDialogState extends State<TwoFactorDialog> {
  final TextEditingController _code = TextEditingController();
  final TextEditingController _password = TextEditingController();
  bool _loading = true;
  bool _loadFailed = false;
  bool _enabled = false;
  bool _busy = false;
  int _recoveryCodes = 0;
  String _secret = '';
  String _uri = '';
  String _error = '';
  String _message = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _code.dispose();
    _password.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _loadFailed = false;
      _error = '';
    });
    try {
      final AccountInfo info = await (widget.loadInfo ?? ServerApi.accountInfo)(
        widget.server,
        widget.account.token,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _loading = false;
        _enabled = info.twoFactor;
        _recoveryCodes = info.recoveryCodes;
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadFailed = true;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _loadFailed = true;
          _error = '读取两步验证状态失败：$error';
        });
      }
    }
  }

  Future<void> _start() async {
    if (_busy) {
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _message = '';
    });
    try {
      final TwoFactorEnrollment enrollment =
          await (widget.start ?? ServerApi.startTwoFactor)(
        widget.server,
        widget.account.token,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _secret = enrollment.secret;
        _uri = enrollment.uri;
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
          _error = '生成密钥失败：$error';
        });
      }
    }
  }

  Future<void> _enable() async {
    if (_busy) {
      return;
    }
    final String code = _code.text.trim();
    if (code.isEmpty) {
      setState(() => _error = '请输入身份验证器中的 6 位验证码');
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await (widget.enable ?? ServerApi.enableTwoFactor)(
        widget.server,
        widget.account.token,
        code,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _enabled = true;
        _recoveryCodes = 0;
        _secret = '';
        _uri = '';
        _code.clear();
        _message = '两步验证已启用';
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
          _error = '启用两步验证失败：$error';
        });
      }
    }
  }

  Future<void> _disable() async {
    if (_busy) {
      return;
    }
    final String password = _password.text;
    final String code = _code.text.trim();
    if (password.isEmpty) {
      setState(() => _error = '请输入当前密码');
      return;
    }
    if (code.isEmpty) {
      setState(() => _error = '请输入身份验证器中的 6 位验证码');
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _message = '';
    });
    try {
      await (widget.disable ?? ServerApi.disableTwoFactor)(
        widget.server,
        widget.account.token,
        password: password,
        code: code,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _enabled = false;
        _recoveryCodes = 0;
        _password.clear();
        _code.clear();
        _message = '两步验证已关闭';
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
          _error = '关闭两步验证失败：$error';
        });
      }
    }
  }

  Future<void> _generateRecovery() async {
    if (_busy) {
      return;
    }
    final String password = _password.text;
    final String code = _code.text.trim();
    if (password.isEmpty) {
      setState(() => _error = '请输入当前密码');
      return;
    }
    if (code.isEmpty) {
      setState(() => _error = '请输入身份验证器中的 6 位验证码');
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _message = '';
    });
    try {
      final List<String> codes =
          await (widget.generateRecoveryCodes ?? ServerApi.generateRecoveryCodes)(
        widget.server,
        widget.account.token,
        password: password,
        code: code,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _recoveryCodes = codes.length;
        _password.clear();
        _code.clear();
        _message = '已生成 ${codes.length} 个恢复码';
      });
      await showDialog<void>(
        context: context,
        builder: (BuildContext context) => _RecoveryCodesDialog(codes: codes),
      );
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
          _error = '生成恢复码失败：$error';
        });
      }
    }
  }

  Widget _codeField({bool autofocus = false}) {
    return TextField(
      key: const Key('two-factor-code'),
      controller: _code,
      autofocus: autofocus,
      keyboardType: TextInputType.number,
      maxLength: 6,
      autocorrect: false,
      enableSuggestions: false,
      decoration: const InputDecoration(
        labelText: '6 位验证码',
        counterText: '',
        prefixIcon: Icon(Icons.shield_outlined, size: 18),
      ),
      onSubmitted: (_) => _enabled ? _disable() : _enable(),
    );
  }

  Widget _statusHeader(BuildContext context, IconData icon, String status) {
    final ThemeData theme = Theme.of(context);
    return Row(
      children: <Widget>[
        Icon(icon, size: 20, color: kAccent),
        const SizedBox(width: 8),
        Text(
          status,
          key: const Key('two-factor-status'),
          style: TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w600,
            color: theme.colorScheme.onSurface,
          ),
        ),
      ],
    );
  }

  Widget _body(BuildContext context) {
    if (_loading) {
      return const Padding(
        padding: EdgeInsets.symmetric(vertical: 24),
        child: Center(child: CircularProgressIndicator(strokeWidth: 2)),
      );
    }
    if (_loadFailed) {
      return Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Text(
            _error,
            key: const Key('two-factor-error'),
            style: TextStyle(
              color: Theme.of(context).colorScheme.error,
              fontSize: 12,
            ),
          ),
          const SizedBox(height: 12),
          OutlinedButton(
            key: const Key('two-factor-retry'),
            onPressed: _load,
            child: const Text('重试'),
          ),
        ],
      );
    }
    if (_enabled) {
      return Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          _statusHeader(context, Icons.verified_user_outlined, '两步验证已启用'),
          const SizedBox(height: 10),
          Text(
            '登录时除密码外还需要身份验证器生成的验证码。关闭需要当前密码和一个有效验证码。',
            style: mutedStyle(context, size: 12),
          ),
          const SizedBox(height: 14),
          TextField(
            key: const Key('two-factor-password'),
            controller: _password,
            obscureText: true,
            autocorrect: false,
            enableSuggestions: false,
            decoration: const InputDecoration(
              labelText: '当前密码',
              prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
            ),
          ),
          const SizedBox(height: 10),
          _codeField(),
          const SizedBox(height: 10),
          Text(
            _recoveryCodes > 0
                ? '剩余恢复码：$_recoveryCodes 个。恢复码用于身份验证器丢失时登录，每个只能使用一次。'
                : '尚未生成恢复码。生成后可在身份验证器丢失时登录。重新生成会让旧恢复码全部失效。',
            key: const Key('two-factor-recovery-count'),
            style: mutedStyle(context, size: 12),
          ),
          const SizedBox(height: 8),
          OutlinedButton.icon(
            key: const Key('two-factor-recovery-generate'),
            onPressed: _busy ? null : _generateRecovery,
            icon: const Icon(Icons.vpn_key_outlined, size: 17),
            label: Text(_busy ? '正在生成…' : '生成恢复码'),
          ),
          const SizedBox(height: 12),
          FilledButton(
            key: const Key('two-factor-disable'),
            onPressed: _busy ? null : _disable,
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(context).colorScheme.error,
            ),
            child: _busy
                ? const SizedBox(
                    width: 15,
                    height: 15,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Text('关闭两步验证'),
          ),
        ],
      );
    }
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        _statusHeader(context, Icons.gpp_maybe_outlined, '两步验证未启用'),
        const SizedBox(height: 10),
        Text(
          '启用后每次登录都需要身份验证器中的验证码。密钥只显示一次，请立即添加；丢失后可请管理员执行 user 2fa <用户名> off 重置。',
          style: mutedStyle(context, size: 12),
        ),
        const SizedBox(height: 14),
        if (_secret.isEmpty)
          OutlinedButton.icon(
            key: const Key('two-factor-start'),
            onPressed: _busy ? null : _start,
            icon: const Icon(Icons.key_outlined, size: 17),
            label: Text(_busy ? '正在生成…' : '生成密钥'),
          )
        else ...<Widget>[
          Text('密钥（手动添加到身份验证器）', style: mutedStyle(context, size: 11)),
          const SizedBox(height: 4),
          SelectableText(
            _secret,
            key: const Key('two-factor-secret'),
            style: const TextStyle(fontFamily: 'monospace', fontSize: 13),
          ),
          const SizedBox(height: 8),
          Text('otpauth URI', style: mutedStyle(context, size: 11)),
          const SizedBox(height: 4),
          SelectableText(
            _uri,
            key: const Key('two-factor-uri'),
            style: const TextStyle(fontFamily: 'monospace', fontSize: 10),
          ),
          const SizedBox(height: 12),
          _codeField(autofocus: true),
          const SizedBox(height: 12),
          FilledButton(
            key: const Key('two-factor-enable'),
            onPressed: _busy ? null : _enable,
            child: _busy
                ? const SizedBox(
                    width: 15,
                    height: 15,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Text('确认启用'),
          ),
        ],
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      key: const Key('two-factor-dialog'),
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: const Text('两步验证'),
      content: SizedBox(
        width: 380,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            _body(context),
            if (_message.isNotEmpty) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _message,
                key: const Key('two-factor-message'),
                style: const TextStyle(color: kAccent, fontSize: 12),
              ),
            ],
            if (_error.isNotEmpty && !_loadFailed) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _error,
                key: const Key('two-factor-error'),
                style: TextStyle(
                  color: Theme.of(context).colorScheme.error,
                  fontSize: 12,
                ),
              ),
            ],
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('two-factor-close'),
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: const Text('关闭'),
        ),
      ],
    );
  }
}

/// Shows a freshly minted recovery-code set once. The server keeps only
/// hashes, so this dialog is the only chance to write the codes down.
class _RecoveryCodesDialog extends StatelessWidget {
  const _RecoveryCodesDialog({required this.codes});

  final List<String> codes;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      key: const Key('recovery-codes-dialog'),
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: const Text('恢复码'),
      content: SizedBox(
        width: 340,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              '每个恢复码只能使用一次，重新生成会让这一组全部失效。请保存在离线、安全的地方。',
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 12),
            SelectableText(
              codes.join('\n'),
              key: const Key('recovery-codes-list'),
              style: const TextStyle(
                fontFamily: 'monospace',
                fontSize: 13,
                height: 1.6,
              ),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('recovery-codes-copy'),
          onPressed: () async {
            await Clipboard.setData(ClipboardData(text: codes.join('\n')));
            if (context.mounted) {
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('恢复码已复制')),
              );
            }
          },
          child: const Text('复制全部'),
        ),
        FilledButton(
          key: const Key('recovery-codes-close'),
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('我已保存'),
        ),
      ],
    );
  }
}
