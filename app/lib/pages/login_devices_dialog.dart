import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../account_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';
import '../viewer_bridge.dart';
import 'package:chast_viewer/l10n.dart';

class LoginDevicesDialog extends StatefulWidget {
  const LoginDevicesDialog({
    super.key,
    required this.server,
    required this.account,
    this.loadDevices,
    this.updatePolicy,
    this.revokeDevice,
  });

  final ServerSettings server;
  final AccountSession account;
  final Future<LoginDeviceState> Function(ServerSettings, String)? loadDevices;
  final Future<LoginDeviceState> Function(
    ServerSettings,
    String, {
    required bool enabled,
  })?
  updatePolicy;
  final Future<void> Function(ServerSettings, String, String)? revokeDevice;

  @override
  State<LoginDevicesDialog> createState() => _LoginDevicesDialogState();
}

class _LoginDevicesDialogState extends State<LoginDevicesDialog> {
  LoginDeviceState? _state;
  bool _loading = true;
  bool _busy = false;
  String _error = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final LoginDeviceState state =
          await (widget.loadDevices ?? ServerApi.loginDevices)(
            widget.server,
            widget.account.token,
          );
      if (mounted) {
        setState(() {
          _state = state;
          _loading = false;
          _error = '';
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = tr('无法读取登录设备：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _setPolicy(bool enabled) async {
    if (_busy) return;
    if (enabled) {
      final bool? confirmed = await showDialog<bool>(
        context: context,
        builder: (BuildContext context) => AlertDialog(
          title: Text(tr('启用登录设备白名单？')),
          content: Text(
            tr('启用后，只有已登记的查看器设备可以登录。其他设备上的现有账号会话也会退出。请确认当前设备已显示在信任列表中。'),
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: Text(tr('取消')),
            ),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(true),
              child: Text(tr('启用')),
            ),
          ],
        ),
      );
      if (confirmed != true) return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      final LoginDeviceState state =
          await (widget.updatePolicy ?? ServerApi.setLoginDevicePolicy)(
            widget.server,
            widget.account.token,
            enabled: enabled,
          );
      if (mounted) setState(() => _state = state);
    } catch (error) {
      if (mounted) {
        setState(() {
          _error = tr('更新登录设备策略失败：{error}', <String, Object?>{'error': error});
        });
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _revoke(TrustedLoginDevice device) async {
    if (_busy || device.current) return;
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        title: Text(tr('撤销登录设备？')),
        content: Text(
          tr('撤销“{name}”后，该设备上的所有账号会话都会立即失效。', <String, Object?>{
            'name': device.deviceName.isEmpty ? device.id : device.deviceName,
          }),
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(tr('取消')),
          ),
          FilledButton(
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(context).colorScheme.error,
            ),
            onPressed: () => Navigator.of(context).pop(true),
            child: Text(tr('撤销')),
          ),
        ],
      ),
    );
    if (confirmed != true) return;
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await (widget.revokeDevice ?? ServerApi.revokeLoginDevice)(
        widget.server,
        widget.account.token,
        device.id,
      );
      await _load();
    } catch (error) {
      if (mounted) {
        setState(() {
          _error = tr('撤销登录设备失败：{error}', <String, Object?>{'error': error});
        });
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _copyPublicKey() async {
    try {
      final String publicKey = (await ViewerBridge.ticketIdentity()).trim();
      if (publicKey.isEmpty) throw StateError('empty public key');
      await Clipboard.setData(ClipboardData(text: publicKey));
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(tr('本机登录设备公钥已复制'))));
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _error = tr('无法复制本机登录设备公钥：{error}', <String, Object?>{
            'error': error,
          });
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final LoginDeviceState? state = _state;
    return AlertDialog(
      title: Text(tr('登录设备白名单')),
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      content: SizedBox(
        width: 440,
        child: _loading
            ? const SizedBox(
                height: 90,
                child: Center(child: CircularProgressIndicator()),
              )
            : state == null
            ? Text(_error, style: TextStyle(color: theme.colorScheme.error))
            : SingleChildScrollView(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    Text(
                      tr('关闭时，新登录设备会自动加入信任列表。开启后，仅列表中的设备可以登录；管理员可通过账号管理恢复访问。'),
                      style: mutedStyle(context, size: 12),
                    ),
                    const SizedBox(height: 8),
                    SwitchListTile.adaptive(
                      key: const Key('login-device-allowlist'),
                      contentPadding: EdgeInsets.zero,
                      value: state.enabled,
                      onChanged: _busy ? null : _setPolicy,
                      title: Text(tr('仅允许信任设备登录')),
                      subtitle: Text(
                        state.enabled ? tr('白名单已启用') : tr('白名单已关闭'),
                        style: mutedStyle(context, size: 11),
                      ),
                    ),
                    Align(
                      alignment: Alignment.centerLeft,
                      child: TextButton.icon(
                        key: const Key('login-device-copy-public-key'),
                        onPressed: _busy ? null : _copyPublicKey,
                        icon: const Icon(Icons.copy_rounded, size: 16),
                        label: Text(tr('复制本机登录设备公钥')),
                      ),
                    ),
                    const Divider(height: 20),
                    if (state.devices.isEmpty)
                      Text(
                        state.enabled
                            ? tr('白名单已启用但没有登记设备；请联系管理员恢复访问。')
                            : tr('此账号没有已登记设备；请退出并重新登录以登记此设备。'),
                        style: mutedStyle(context, size: 12),
                      )
                    else
                      for (final TrustedLoginDevice device in state.devices)
                        ListTile(
                          contentPadding: EdgeInsets.zero,
                          leading: Icon(
                            device.current
                                ? Icons.computer_rounded
                                : Icons.devices_outlined,
                            color: device.current ? kAccent : null,
                          ),
                          title: Text(
                            device.deviceName.isEmpty
                                ? tr('未命名设备')
                                : device.deviceName,
                          ),
                          subtitle: Text(
                            device.current
                                ? tr('当前设备 · ID {id}', <String, Object?>{
                                    'id': device.id,
                                  })
                                : tr('ID {id}', <String, Object?>{
                                    'id': device.id,
                                  }),
                            style: mutedStyle(context, size: 10),
                          ),
                          trailing: IconButton(
                            tooltip: tr('撤销设备'),
                            onPressed: _busy || device.current
                                ? null
                                : () => _revoke(device),
                            icon: const Icon(Icons.remove_circle_outline),
                          ),
                        ),
                    if (_error.isNotEmpty) ...<Widget>[
                      const SizedBox(height: 8),
                      Text(
                        _error,
                        key: const Key('login-device-error'),
                        style: TextStyle(color: theme.colorScheme.error),
                      ),
                    ],
                  ],
                ),
              ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: Text(tr('关闭')),
        ),
      ],
    );
  }
}
