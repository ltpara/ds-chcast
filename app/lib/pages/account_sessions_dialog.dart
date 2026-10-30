import 'package:flutter/material.dart';

import '../account_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';
import 'package:chast_viewer/l10n.dart';

typedef AccountSessionsLoader =
    Future<List<AccountDeviceSession>> Function(
      ServerSettings settings,
      String token,
    );
typedef AccountSessionRevoker =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String sessionId,
    );
typedef AccountOtherSessionsRevoker =
    Future<int> Function(ServerSettings settings, String token);
typedef AccountSessionRenamer =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String sessionId,
      String deviceName,
    );

/// Lists every server-side login session of the signed-in account and lets
/// the user rename devices and sign other devices out, one at a time or all
/// at once. The current session is marked and cannot be revoked here; leaving
/// this device uses the normal account logout.
class AccountSessionsDialog extends StatefulWidget {
  const AccountSessionsDialog({
    super.key,
    required this.server,
    required this.account,
    this.loadSessions,
    this.revokeSession,
    this.revokeOtherSessions,
    this.renameSession,
  });

  final ServerSettings server;
  final AccountSession account;
  final AccountSessionsLoader? loadSessions;
  final AccountSessionRevoker? revokeSession;
  final AccountOtherSessionsRevoker? revokeOtherSessions;
  final AccountSessionRenamer? renameSession;

  @override
  State<AccountSessionsDialog> createState() => _AccountSessionsDialogState();
}

class _AccountSessionsDialogState extends State<AccountSessionsDialog> {
  List<AccountDeviceSession> _sessions = const <AccountDeviceSession>[];
  bool _loading = true;
  bool _revokingOthers = false;
  String _error = '';
  String _revoking = '';
  String _renaming = '';

  bool get _busy =>
      _revoking.isNotEmpty || _revokingOthers || _renaming.isNotEmpty;

  bool get _hasOtherSessions =>
      _sessions.any((AccountDeviceSession session) => !session.current);

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _error = '';
    });
    try {
      final List<AccountDeviceSession> sessions =
          await (widget.loadSessions ?? ServerApi.accountSessions)(
            widget.server,
            widget.account.token,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _sessions = sessions;
        _loading = false;
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = tr('读取设备会话失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _revoke(AccountDeviceSession session) async {
    if (_busy) {
      return;
    }
    setState(() {
      _revoking = session.id;
      _error = '';
    });
    try {
      await (widget.revokeSession ?? ServerApi.revokeAccountSession)(
        widget.server,
        widget.account.token,
        session.id,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _sessions = _sessions
            .where((AccountDeviceSession other) => other.id != session.id)
            .toList();
        _revoking = '';
      });
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(tr('已退出该设备'))));
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _revoking = '';
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _revoking = '';
          _error = tr('退出设备失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _rename(AccountDeviceSession session) async {
    if (_busy) {
      return;
    }
    final String? name = await showDialog<String>(
      context: context,
      builder: (BuildContext context) =>
          _RenameSessionDialog(initial: session.deviceName),
    );
    if (name == null || !mounted) {
      return;
    }
    setState(() {
      _renaming = session.id;
      _error = '';
    });
    try {
      await (widget.renameSession ?? ServerApi.renameAccountSession)(
        widget.server,
        widget.account.token,
        session.id,
        name,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _sessions = _sessions
            .map(
              (AccountDeviceSession other) => other.id == session.id
                  ? other.copyWith(deviceName: name)
                  : other,
            )
            .toList();
        _renaming = '';
      });
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(tr('已更新设备名称'))));
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _renaming = '';
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _renaming = '';
          _error = tr('重命名设备失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _revokeOthers() async {
    if (_busy) {
      return;
    }
    setState(() {
      _revokingOthers = true;
      _error = '';
    });
    try {
      final int count =
          await (widget.revokeOtherSessions ??
              ServerApi.revokeOtherAccountSessions)(
            widget.server,
            widget.account.token,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _sessions = _sessions
            .where((AccountDeviceSession session) => session.current)
            .toList();
        _revokingOthers = false;
      });
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(count > 0 ? tr('已退出 {count} 台其他设备', <String, Object?>{'count': count}) : tr('没有其他已登录设备'))),
      );
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _revokingOthers = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _revokingOthers = false;
          _error = tr('退出其他设备失败：{error}', <String, Object?>{'error': error});
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
      title: Text(tr('设备会话')),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              tr('这些设备正使用当前账号登录。退出后该设备需要重新登录，地址簿同步也会停止。'),
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 14),
            if (_loading)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 28),
                child: Center(child: CircularProgressIndicator()),
              )
            else if (_sessions.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 18),
                child: Text(
                  tr('没有其他已登录设备。'),
                  key: const Key('account-sessions-empty'),
                  style: mutedStyle(context, size: 12),
                ),
              )
            else
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 260),
                child: ListView.separated(
                  key: const Key('account-sessions'),
                  shrinkWrap: true,
                  itemCount: _sessions.length,
                  separatorBuilder: (BuildContext context, int index) =>
                      const SizedBox(height: 8),
                  itemBuilder: (BuildContext context, int index) {
                    return _sessionTile(context, _sessions[index]);
                  },
                ),
              ),
            if (_error.isNotEmpty) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _error,
                key: const Key('account-sessions-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 12),
              ),
            ],
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('account-sessions-revoke-others'),
          onPressed: _loading || _busy || !_hasOtherSessions
              ? null
              : _revokeOthers,
          style: TextButton.styleFrom(foregroundColor: theme.colorScheme.error),
          child: _revokingOthers
              ? const SizedBox(
                  width: 14,
                  height: 14,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : Text(tr('退出其他设备')),
        ),
        TextButton(
          key: const Key('account-sessions-refresh'),
          onPressed: _loading || _busy ? null : _load,
          child: Text(tr('刷新')),
        ),
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('关闭')),
        ),
      ],
    );
  }

  Widget _sessionTile(BuildContext context, AccountDeviceSession session) {
    final ThemeData theme = Theme.of(context);
    final String name = session.deviceName.isEmpty
        ? tr('未知设备')
        : session.deviceName;
    final String origin = session.location.isEmpty
        ? session.loginIp
        : (session.loginIp.isEmpty
              ? session.location
              : '${session.location} · ${session.loginIp}');
    return Container(
      key: ValueKey<String>('account-session-${session.id}'),
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceContainerHighest.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Icon(
            session.current
                ? Icons.computer_rounded
                : Icons.devices_other_rounded,
            size: 18,
            color: session.current
                ? kAccent
                : theme.colorScheme.onSurfaceVariant,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Row(
                  children: <Widget>[
                    Flexible(
                      child: Text(
                        name,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          fontSize: 13,
                          fontWeight: FontWeight.w600,
                          color: theme.colorScheme.onSurface,
                        ),
                      ),
                    ),
                    if (session.current) ...<Widget>[
                      const SizedBox(width: 6),
                      Text(
                        tr('当前设备'),
                        style: TextStyle(
                          fontSize: 10.5,
                          color: kAccent,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                    ],
                  ],
                ),
                if (session.deviceId.isNotEmpty)
                  Text(
                    tr('设备 ID {p4}', <String, Object?>{'p4': session.deviceId}),
                    style: mutedStyle(context, size: 10.5),
                  ),
                Text(
                  tr('登录于 {p5} · 有效期至 {p6}', <String, Object?>{'p5': _formatSessionTime(session.createdAt.toLocal()), 'p6': _formatSessionTime(session.expiresAt.toLocal())}),
                  style: mutedStyle(context, size: 10.5),
                ),
                if (origin.isNotEmpty)
                  Text(tr('登录地点 {origin}', <String, Object?>{'origin': origin}), style: mutedStyle(context, size: 10.5)),
              ],
            ),
          ),
          Row(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              IconButton(
                key: ValueKey<String>('account-session-rename-${session.id}'),
                onPressed: _busy ? null : () => _rename(session),
                tooltip: tr('重命名'),
                visualDensity: VisualDensity.compact,
                padding: EdgeInsets.zero,
                constraints: const BoxConstraints(minWidth: 30, minHeight: 30),
                icon: _renaming == session.id
                    ? const SizedBox(
                        width: 13,
                        height: 13,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.edit_outlined, size: 16),
              ),
              if (!session.current)
                TextButton(
                  key: ValueKey<String>('account-session-revoke-${session.id}'),
                  onPressed: _busy ? null : () => _revoke(session),
                  style: TextButton.styleFrom(
                    foregroundColor: theme.colorScheme.error,
                    minimumSize: const Size(48, 30),
                    padding: const EdgeInsets.symmetric(horizontal: 8),
                  ),
                  child: _revoking == session.id
                      ? const SizedBox(
                          width: 13,
                          height: 13,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : Text(tr('退出'), style: TextStyle(fontSize: 12)),
                ),
            ],
          ),
        ],
      ),
    );
  }
}

String _formatSessionTime(DateTime time) {
  String two(int value) => value.toString().padLeft(2, '0');
  return '${time.year}-${two(time.month)}-${two(time.day)} '
      '${two(time.hour)}:${two(time.minute)}';
}

class _RenameSessionDialog extends StatefulWidget {
  const _RenameSessionDialog({required this.initial});

  final String initial;

  @override
  State<_RenameSessionDialog> createState() => _RenameSessionDialogState();
}

class _RenameSessionDialogState extends State<_RenameSessionDialog> {
  late final TextEditingController _name = TextEditingController(
    text: widget.initial,
  );
  String _error = '';

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  void _save() {
    final String name = _name.text.trim();
    if (name.isEmpty) {
      setState(() => _error = tr('请输入设备名称'));
      return;
    }
    Navigator.of(context).pop(name);
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      title: Text(tr('重命名设备')),
      content: SizedBox(
        width: 320,
        child: TextField(
          key: const Key('account-session-rename-field'),
          controller: _name,
          autofocus: true,
          maxLength: 128,
          decoration: InputDecoration(
            labelText: tr('设备名称'),
            hintText: tr('例如：书房 PC'),
            errorText: _error.isEmpty ? null : _error,
          ),
          onSubmitted: (_) => _save(),
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('account-session-rename-save'),
          onPressed: _save,
          child: Text(tr('保存')),
        ),
      ],
    );
  }
}
