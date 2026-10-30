import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'target.dart';
import 'tunnel_agent.dart';
import 'package:chast_viewer/l10n.dart';

/// One host that answered a LAN discovery ping.
class LanPeer {
  const LanPeer({
    required this.id,
    this.address = '',
    this.addresses = const <String>[],
    this.hostname = '',
    this.username = '',
    this.platform = '',
    this.version = '',
    this.mac = '',
  });

  final String id;
  final String address;
  final List<String> addresses;
  final String hostname;
  final String username;
  final String platform;
  final String version;
  final String mac;

  String get label => hostname.isEmpty ? id : hostname;

  String get detail {
    final List<String> parts = <String>[
      id,
      if (address.isNotEmpty) address,
      if (platform.isNotEmpty) platform,
    ];
    return parts.join(' · ');
  }

  LanPeer merge(LanPeer other) {
    final List<String> merged = <String>[];
    for (final String item in <String>[...addresses, address, ...other.addresses, other.address]) {
      if (item.isNotEmpty && !merged.contains(item)) {
        merged.add(item);
      }
    }
    merged.sort((String a, String b) {
      final bool loopA = _isLoopback(a);
      final bool loopB = _isLoopback(b);
      if (loopA != loopB) {
        return loopA ? 1 : -1;
      }
      return 0;
    });
    return LanPeer(
      id: id,
      address: merged.isEmpty ? '' : merged.first,
      addresses: merged,
      hostname: hostname.isEmpty ? other.hostname : hostname,
      username: username.isEmpty ? other.username : username,
      platform: platform.isEmpty ? other.platform : platform,
      version: version.isEmpty ? other.version : version,
      mac: mac.isEmpty ? other.mac : mac,
    );
  }

  static LanPeer? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String id = _field(value['id'], 64);
    if (!isValidHostId(id)) {
      return null;
    }
    final List<String> addresses = <String>[
      if (value['addresses'] is List)
        for (final Object? item in value['addresses'] as List)
          if (_field(item, 96).isNotEmpty) _field(item, 96),
    ];
    final String primary = _field(value['address'], 96);
    if (primary.isNotEmpty && !addresses.contains(primary)) {
      addresses.insert(0, primary);
    }
    return LanPeer(
      id: id,
      address: primary.isNotEmpty
          ? primary
          : (addresses.isEmpty ? '' : addresses.first),
      addresses: addresses,
      hostname: _field(value['hostname'], 128),
      username: _field(value['username'], 128),
      platform: _field(value['platform'], 64),
      version: _field(value['version'], 64),
      mac: _field(value['mac'], 32),
    );
  }

  static String _field(Object? value, int max) {
    if (value is! String) {
      return '';
    }
    final String text = value.trim();
    return text.length <= max ? text : text.substring(0, max);
  }

  static bool _isLoopback(String address) {
    var host = address;
    if (host.startsWith('[')) {
      final int end = host.indexOf(']');
      if (end > 0) {
        host = host.substring(1, end);
      }
    } else {
      final int colon = host.lastIndexOf(':');
      if (colon > 0) {
        host = host.substring(0, colon);
      }
    }
    return host == '127.0.0.1' || host == '::1' || host == 'localhost';
  }
}

class LanDiscoveryException implements Exception {
  const LanDiscoveryException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Runs the bundled `chast-tunnel discover --json` agent.
///
/// Discovery is a viewer-side capability: it does not need a configured
/// connection service, but adding an answer to the address book does, because
/// the pong carries a host id rather than the host's QUIC port.
class LanDiscovery {
  const LanDiscovery({this.executable, this.runner});

  static const Duration defaultTimeout = Duration(seconds: 2);

  /// Overrides the sibling `chast-tunnel.exe` lookup; `CHAST_TUNNEL` is
  /// honored by [TunnelAgent.defaultExecutable].
  final String? executable;

  /// Test seam. Product code uses [Process.run].
  final Future<ProcessResult> Function(String executable, List<String> arguments)?
      runner;

  Future<List<LanPeer>> scan({
    String selfId = '',
    Duration timeout = defaultTimeout,
  }) async {
    final String exe = executable ?? TunnelAgent.defaultExecutable();
    final List<String> arguments = <String>[
      'discover',
      '--json',
      '--timeout',
      '${timeout.inMilliseconds}ms',
      if (selfId.isNotEmpty) ...<String>['--id', selfId],
    ];
    final Future<ProcessResult> pending = runner != null
        ? runner!(exe, arguments)
        : Process.run(
            exe,
            arguments,
            runInShell: false,
            stdoutEncoding: utf8,
            stderrEncoding: utf8,
          );
    final ProcessResult result;
    try {
      result = await pending.timeout(timeout + const Duration(seconds: 5));
    } on ProcessException catch (error) {
      throw LanDiscoveryException(
        tr('无法启动局域网扫描（{exe}）：{p2}\n请确认 chast-tunnel.exe 与会话程序位于同一目录。', <String, Object?>{'exe': exe, 'p2': error.message}),
      );
    } on TimeoutException {
      throw LanDiscoveryException(tr('局域网扫描没有在预期时间内返回'));
    } on Exception catch (error) {
      throw LanDiscoveryException(tr('局域网扫描失败：{error}', <String, Object?>{'error': error}));
    }
    if (result.exitCode != 0) {
      final String message = (result.stderr as String? ?? '').trim();
      throw LanDiscoveryException(
        message.isEmpty ? tr('局域网扫描失败（{p3}）', <String, Object?>{'p3': result.exitCode}) : message,
      );
    }
    Object? decoded;
    try {
      decoded = jsonDecode(result.stdout as String? ?? '');
    } on FormatException {
      throw LanDiscoveryException(tr('局域网扫描返回了无效数据'));
    }
    if (decoded is! List) {
      throw LanDiscoveryException(tr('局域网扫描返回了无效数据'));
    }
    final Map<String, LanPeer> peers = <String, LanPeer>{};
    for (final Object? item in decoded) {
      final LanPeer? peer = LanPeer.fromJson(item);
      if (peer == null) {
        continue;
      }
      final LanPeer? existing = peers[peer.id];
      peers[peer.id] = existing == null ? peer : existing.merge(peer);
    }
    return peers.values.toList();
  }
}
