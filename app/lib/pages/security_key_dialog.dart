import 'package:flutter/material.dart';

import '../account_store.dart';
import '../security_key.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';

typedef SecurityKeyAvailabilityProbe = Future<bool> Function();
typedef SecurityKeyCredentialsLoader =
    Future<List<WebAuthnCredential>> Function(
      ServerSettings settings,
      String token,
    );
typedef SecurityKeyRegistrationStarter =
    Future<WebAuthnRegisterOptions> Function(
      ServerSettings settings,
      String token, {
      required String password,
    });
typedef SecurityKeyRegistrationFinisher =
    Future<WebAuthnCredential> Function(
      ServerSettings settings,
      String token, {
      required String name,
      required String clientDataJSON,
      required String attestationObject,
    });
typedef SecurityKeyRemover =
    Future<void> Function(
      ServerSettings settings,
      String token, {
      required String password,
      required String credentialId,
    });
typedef SecurityKeyMaker =
    Future<SecurityKeyMakeResult> Function({
      required String rpId,
      required String userId,
      required String username,
      required String displayName,
      required String challenge,
      required List<String> excludeCredentials,
      required int timeoutMs,
    });

/// Lists, adds and removes Windows Hello / FIDO2 credentials for the signed-in
/// account. Every change re-checks the password so a borrowed token cannot add
/// or drop a login factor.
class SecurityKeyDialog extends StatefulWidget {
  const SecurityKeyDialog({
    super.key,
    required this.server,
    required this.account,
    this.probeAvailability,
    this.loadCredentials,
    this.startRegistration,
    this.finishRegistration,
    this.removeCredential,
    this.makeCredential,
  });

  final ServerSettings server;
  final AccountSession account;
  final SecurityKeyAvailabilityProbe? probeAvailability;
  final SecurityKeyCredentialsLoader? loadCredentials;
  final SecurityKeyRegistrationStarter? startRegistration;
  final SecurityKeyRegistrationFinisher? finishRegistration;
  final SecurityKeyRemover? removeCredential;
  final SecurityKeyMaker? makeCredential;

  @override
  State<SecurityKeyDialog> createState() => _SecurityKeyDialogState();
}

class _SecurityKeyDialogState extends State<SecurityKeyDialog> {
  final SecurityKey _key = SecurityKey();
  final TextEditingController _password = TextEditingController();
  final TextEditingController _name = TextEditingController();
  List<WebAuthnCredential> _credentials = <WebAuthnCredential>[];
  bool _loading = true;
  bool _loadFailed = false;
  bool _busy = false;
  bool _available = false;
  String _error = '';
  String _message = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _password.dispose();
    _name.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _loadFailed = false;
      _error = '';
    });
    try {
      final List<WebAuthnCredential> credentials =
          await (widget.loadCredentials ?? ServerApi.webauthnCredentials)(
        widget.server,
        widget.account.token,
      );
      bool available = false;
      try {
        available =
            await (widget.probeAvailability ?? _key.available)();
      } on SecurityKeyException {
        available = false;
      }
      if (!mounted) {
        return;
      }
      setState(() {
        _loading = false;
        _credentials = credentials;
        _available = available;
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
          _error = '读取安全密钥失败：$error';
        });
      }
    }
  }

  Future<void> _register() async {
    if (_busy) {
      return;
    }
    final String password = _password.text;
    if (password.isEmpty) {
      setState(() => _error = '请输入当前密码');
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _message = '';
    });
    try {
      final WebAuthnRegisterOptions options =
          await (widget.startRegistration ?? ServerApi.webauthnRegisterBegin)(
        widget.server,
        widget.account.token,
        password: password,
      );
      final SecurityKeyMakeResult made =
          await (widget.makeCredential ?? _makeCredential)(
        rpId: options.rpId,
        userId: options.userId,
        username: options.username,
        displayName: options.displayName,
        challenge: options.challenge,
        excludeCredentials: <String>[
          for (final WebAuthnCredential credential in options.excludeCredentials)
            credential.id,
        ],
        timeoutMs: options.timeoutMs,
      );
      final String name = _name.text.trim();
      final WebAuthnCredential stored =
          await (widget.finishRegistration ?? ServerApi.webauthnRegisterFinish)(
        widget.server,
        widget.account.token,
        name: name.isEmpty ? '此电脑' : name,
        clientDataJSON: made.clientDataJSON,
        attestationObject: made.attestationObject,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _credentials = <WebAuthnCredential>[..._credentials, stored];
        _available = true;
        _password.clear();
        _name.clear();
        _message = '安全密钥已添加';
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
    } on SecurityKeyException catch (error) {
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
          _error = '注册安全密钥失败：$error';
        });
      }
    }
  }

  Future<SecurityKeyMakeResult> _makeCredential({
    required String rpId,
    required String userId,
    required String username,
    required String displayName,
    required String challenge,
    required List<String> excludeCredentials,
    required int timeoutMs,
  }) {
    return _key.makeCredential(
      rpId: rpId,
      userId: userId,
      username: username,
      displayName: displayName,
      challenge: challenge,
      excludeCredentials: excludeCredentials,
      timeoutMs: timeoutMs,
    );
  }

  Future<void> _remove(WebAuthnCredential credential) async {
    if (_busy) {
      return;
    }
    final String password = _password.text;
    if (password.isEmpty) {
      setState(() => _error = '删除安全密钥需要当前密码');
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _message = '';
    });
    try {
      await (widget.removeCredential ?? ServerApi.webauthnRemove)(
        widget.server,
        widget.account.token,
        password: password,
        credentialId: credential.id,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _credentials = <WebAuthnCredential>[
          for (final WebAuthnCredential entry in _credentials)
            if (entry.id != credential.id) entry,
        ];
        _password.clear();
        _message = '安全密钥已删除';
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
          _error = '删除安全密钥失败：$error';
        });
      }
    }
  }

  Widget _statusHeader(BuildContext context, IconData icon, String status) {
    final ThemeData theme = Theme.of(context);
    return Row(
      children: <Widget>[
        Icon(icon, size: 20, color: kAccent),
        const SizedBox(width: 8),
        Text(
          status,
          key: const Key('security-key-status'),
          style: TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w600,
            color: theme.colorScheme.onSurface,
          ),
        ),
      ],
    );
  }

  Widget _credentialRow(BuildContext context, int index, WebAuthnCredential credential) {
    final ThemeData theme = Theme.of(context);
    final String name = credential.name.isEmpty ? '安全密钥' : credential.name;
    final String created = credential.createdAtMs > 0
        ? _formatDate(DateTime.fromMillisecondsSinceEpoch(
            credential.createdAtMs,
            isUtc: true,
          ).toLocal())
        : '';
    final String used = credential.lastUsedAtMs > 0
        ? '上次使用 ${_formatDate(DateTime.fromMillisecondsSinceEpoch(
            credential.lastUsedAtMs,
            isUtc: true,
          ).toLocal())}'
        : '尚未使用';
    return Container(
      key: Key('security-key-row-$index'),
      margin: const EdgeInsets.only(bottom: 8),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        border: Border.all(color: theme.colorScheme.outlineVariant),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Row(
        children: <Widget>[
          const Icon(Icons.fingerprint_rounded, size: 20, color: kAccent),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  name,
                  style: TextStyle(
                    fontSize: 13,
                    fontWeight: FontWeight.w600,
                    color: theme.colorScheme.onSurface,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  created.isEmpty ? used : '添加于 $created · $used',
                  style: mutedStyle(context, size: 10.5),
                ),
              ],
            ),
          ),
          IconButton(
            key: Key('security-key-remove-$index'),
            tooltip: '删除',
            onPressed: _busy ? null : () => _remove(credential),
            icon: Icon(
              Icons.delete_outline_rounded,
              size: 18,
              color: theme.colorScheme.error,
            ),
          ),
        ],
      ),
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
            key: const Key('security-key-error'),
            style: TextStyle(
              color: Theme.of(context).colorScheme.error,
              fontSize: 12,
            ),
          ),
          const SizedBox(height: 12),
          OutlinedButton(
            key: const Key('security-key-retry'),
            onPressed: _load,
            child: const Text('重试'),
          ),
        ],
      );
    }
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        _statusHeader(
          context,
          _available ? Icons.verified_user_outlined : Icons.gpp_maybe_outlined,
          _available ? '此设备支持 Windows Hello' : '此设备未配置 Windows Hello',
        ),
        const SizedBox(height: 8),
        Text(
          _available
              ? '登录时先校验密码，再用 Windows Hello 确认身份。'
              : '添加安全密钥需要在此设备配置 PIN 或生物识别。已注册的密钥仍可在其他设备使用。',
          style: mutedStyle(context, size: 12),
        ),
        const SizedBox(height: 14),
        if (_credentials.isEmpty)
          Text(
            '尚未注册安全密钥',
            key: const Key('security-key-empty'),
            style: mutedStyle(context, size: 12),
          )
        else
          for (int index = 0; index < _credentials.length; ++index)
            _credentialRow(context, index, _credentials[index]),
        const SizedBox(height: 10),
        Text('添加安全密钥', style: mutedStyle(context, size: 11)),
        const SizedBox(height: 6),
        TextField(
          key: const Key('security-key-password'),
          controller: _password,
          obscureText: true,
          autocorrect: false,
          enableSuggestions: false,
          decoration: const InputDecoration(
            labelText: '当前密码',
            prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
          ),
        ),
        const SizedBox(height: 8),
        TextField(
          key: const Key('security-key-name'),
          controller: _name,
          autocorrect: false,
          enableSuggestions: false,
          decoration: const InputDecoration(
            labelText: '名称（可选）',
            hintText: '例如：此电脑',
            prefixIcon: Icon(Icons.label_outline_rounded, size: 18),
          ),
        ),
        const SizedBox(height: 10),
        OutlinedButton.icon(
          key: const Key('security-key-register'),
          onPressed: _busy ? null : _register,
          icon: const Icon(Icons.fingerprint_rounded, size: 17),
          label: Text(_busy ? '正在等待 Windows Hello…' : '使用 Windows Hello 注册'),
        ),
        const SizedBox(height: 6),
        Text(
          '删除安全密钥同样需要当前密码。注册和删除都会记录到账号审计。',
          style: mutedStyle(context, size: 10.5),
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      key: const Key('security-key-dialog'),
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: const Text('安全密钥'),
      content: SizedBox(
        width: 400,
        child: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              _body(context),
              if (_message.isNotEmpty) ...<Widget>[
                const SizedBox(height: 10),
                Text(
                  _message,
                  key: const Key('security-key-message'),
                  style: const TextStyle(color: kAccent, fontSize: 12),
                ),
              ],
              if (_error.isNotEmpty && !_loadFailed) ...<Widget>[
                const SizedBox(height: 10),
                Text(
                  _error,
                  key: const Key('security-key-error'),
                  style: TextStyle(
                    color: theme.colorScheme.error,
                    fontSize: 12,
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('security-key-close'),
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: const Text('关闭'),
        ),
      ],
    );
  }
}

String _formatDate(DateTime value) {
  String two(int number) => number.toString().padLeft(2, '0');
  return '${value.year}-${two(value.month)}-${two(value.day)}';
}
