import 'dart:convert';

import 'package:flutter/services.dart';

import 'account_store.dart';
import 'server_settings.dart';
import 'target.dart';

/// Passed directly between Flutter engines in this process. Credentials never
/// appear in a process command line or a temporary launch file.
class SessionWindowRequest {
  const SessionWindowRequest({
    required this.target,
    this.account,
    this.settings = const ServerSettings(),
    this.filesOnly = false,
    this.display = 65535,
  });

  final ConnectTarget target;
  final AccountSession? account;
  final ServerSettings settings;
  final bool filesOnly;
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
    'socks5': target.socks5,
    'account': account?.toJson(),
    'settings': settings.toJson(),
    'filesOnly': filesOnly,
    'display': display,
  });

  static SessionWindowRequest? fromArguments(List<String> args) {
    if (args.length != 2 || args.first != '--session-window') return null;
    try {
      final Map<String, dynamic> map =
          jsonDecode(args[1]) as Map<String, dynamic>;
      return SessionWindowRequest(
        target: ConnectTarget(
          map['host'] as String,
          map['port'] as int,
          token: map['token'] as String,
          server: map['server'] as String,
          hostId: map['hostId'] as String,
          displayName: map['name'] as String,
          mode: TunnelModeName.parse(map['mode']),
          serverKey: map['serverKey'] as String,
          socks5: map['socks5'] as String,
        ),
        account: AccountSession.fromJson(map['account']),
        settings: ServerSettings.fromJson(map['settings']),
        filesOnly: map['filesOnly'] == true,
        display: map['display'] as int? ?? 65535,
      );
    } catch (_) {
      return null;
    }
  }

  Future<bool> open() async {
    try {
      return await const MethodChannel(
            'chast/viewer',
          ).invokeMethod<bool>('openSessionWindow', encode()) ??
          false;
    } on MissingPluginException {
      return false;
    }
  }

  static Future<void> close() => const MethodChannel(
    'chast/viewer',
  ).invokeMethod<void>('closeSessionWindow');
}
