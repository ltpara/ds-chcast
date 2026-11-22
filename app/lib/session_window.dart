import 'dart:convert';

import 'package:flutter/services.dart';

import 'account_store.dart';
import 'server_settings.dart';
import 'target.dart';

/// Non-secret recording ownership metadata passed to a display child engine.
/// The account token remains in the normal local account store.
class RecordingUploadContext {
  const RecordingUploadContext({
    required this.server,
    required this.username,
    required this.hostId,
    required this.peerId,
  });

  final String server;
  final String username;
  final String hostId;
  final String peerId;

  Map<String, Object?> toJson() => <String, Object?>{
    'server': server,
    'username': username,
    'host_id': hostId,
    'peer_id': peerId,
  };

  static RecordingUploadContext? fromJson(Object? value) {
    if (value is! Map) return null;
    String string(String key) => value[key]?.toString().trim() ?? '';
    final String server = normalizeServerUrl(string('server'));
    final String username = string('username');
    if (server.isEmpty || username.isEmpty) return null;
    return RecordingUploadContext(
      server: server,
      username: username,
      hostId: string('host_id'),
      peerId: string('peer_id'),
    );
  }
}

/// Passed directly between Flutter engines in this process. Credentials never
/// appear in a process command line or a temporary launch file.
class SessionWindowRequest {
  const SessionWindowRequest({
    required this.target,
    this.account,
    this.settings = const ServerSettings(),
    this.filesOnly = false,
    this.sessionMode = 'desktop',
    this.rdpPort = 0,
    this.display = 65535,
  });

  final ConnectTarget target;
  final AccountSession? account;
  final ServerSettings settings;
  final bool filesOnly;
  final String sessionMode;
  final int rdpPort;
  final int display;

  String encode() => jsonEncode(<String, Object?>{
    'host': target.host,
    'port': target.port,
    'token': target.token,
    'server': target.server,
    'hostId': target.hostId,
    'name': target.displayName,
    'mode': target.mode.name,
    'serverKey': target.serverKey,
    'linkKey': target.linkKey,
    'account': account?.toJson(),
    'settings': settings.toJson(),
    'filesOnly': filesOnly,
    'sessionMode': sessionMode,
    'rdpPort': rdpPort,
    'display': display,
  });

  String get tabLabel {
    final String name = target.displayName.trim();
    if (name.isNotEmpty) return name;
    if (target.hostId.isNotEmpty) return target.hostId;
    if (target.host.isNotEmpty) return '${target.host}:${target.port}';
    return '会话';
  }

  static SessionWindowRequest? fromArguments(List<String> args) {
    if (args.length != 2 || args.first != '--session-window') {
      return null;
    }
    try {
      final Map<String, dynamic> map =
          jsonDecode(args[1]) as Map<String, dynamic>;
      return SessionWindowRequest(
        target: ConnectTarget(
          map['host'] as String,
          map['port'] as int,
          token: map['token'] as String? ?? '',
          server: map['server'] as String? ?? '',
          hostId: map['hostId'] as String? ?? '',
          displayName: map['name'] as String? ?? '',
          mode: TunnelModeName.parse(map['mode']),
          serverKey: map['serverKey'] as String? ?? '',
          linkKey: map['linkKey'] as String? ?? '',
        ),
        account: AccountSession.fromJson(map['account']),
        settings: ServerSettings.fromJson(map['settings']),
        filesOnly: map['filesOnly'] == true,
        sessionMode: map['sessionMode'] as String? ?? 'desktop',
        rdpPort: (map['rdpPort'] as num?)?.toInt() ?? 0,
        display: (map['display'] as num?)?.toInt() ?? 65535,
      );
    } catch (_) {
      return null;
    }
  }

  Future<bool> open() async {
    try {
      return await _channel.invokeMethod<bool>(
            'openSessionWindow',
            <String, Object?>{'request': encode(), 'label': tabLabel},
          ) ??
          false;
    } on MissingPluginException {
      return false;
    }
  }

  static Future<void> close() =>
      _channel.invokeMethod<void>('closeSessionWindow');

  static const MethodChannel _channel = MethodChannel('chast/viewer');
}
