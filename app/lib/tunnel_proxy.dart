import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/l10n.dart';

import 'server_settings.dart';
import 'tunnel_agent.dart';

class ServerProxyException implements Exception {
  const ServerProxyException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// One `chast-tunnel proxy` child process. Dart's HttpClient can only speak
/// the HTTP proxy protocol, so the helper accepts local HTTP proxy
/// connections and forwards each one through the configured upstream (HTTP,
/// HTTPS or SOCKS5). Every API request goes through this bridge when a proxy
/// is configured; a failed bridge is an error, never a reason to dial the
/// server directly.
class ServerProxyBridge {
  ServerProxyBridge._(this._process, this._url);

  static const String host = '127.0.0.1';
  static const List<String> _arguments = <String>[
    'proxy',
    '--proxy-stdin',
    '--listen',
    '127.0.0.1:0',
  ];

  static final Map<String, Future<ServerProxyBridge>> _bridges =
      <String, Future<ServerProxyBridge>>{};

  final Process _process;
  final String _url;
  final List<String> _logs = <String>[];
  final Completer<void> _exited = Completer<void>();
  StreamSubscription<String>? _stdout;
  StreamSubscription<String>? _stderr;
  int _port = 0;
  bool _stopping = false;

  int get port => _port;

  /// "127.0.0.1:port", ready for `HttpClient.findProxy`.
  String get address => '$host:$_port';

  String get url => _url;

  /// The process arguments; exposed for the argument test.
  static List<String> arguments() => List<String>.unmodifiable(_arguments);

  /// Starts or reuses the bridge for [proxy]. Cache entries are keyed by the
  /// normalized URL, so changing the setting stops the previous child instead
  /// of leaving it, and its credentials, running.
  static Future<ServerProxyBridge> ensure(
    String proxy, {
    String? executable,
    Duration timeout = const Duration(seconds: 10),
  }) {
    final String url;
    try {
      url = effectiveProxyUrl(proxy);
    } on FormatException catch (error) {
      throw ServerProxyException(error.message);
    }
    if (url.isEmpty) {
      throw ServerProxyException(tr('出站代理地址为空'));
    }
    final Future<ServerProxyBridge>? cached = _bridges[url];
    if (cached != null) {
      return cached;
    }
    final List<String> stale = <String>[
      for (final String key in _bridges.keys)
        if (key != url) key,
    ];
    for (final String key in stale) {
      final Future<ServerProxyBridge>? old = _bridges.remove(key);
      if (old != null) {
        unawaited(
          old.then<void>(
            (ServerProxyBridge bridge) => bridge.stop(),
            onError: (Object _) {},
          ),
        );
      }
    }
    final Future<ServerProxyBridge> started = _start(
      url,
      executable ?? TunnelAgent.defaultExecutable(),
      timeout,
    );
    _bridges[url] = started;
    unawaited(
      started.then<void>(
        (ServerProxyBridge bridge) async {
          await bridge._exited.future;
          if (identical(_bridges[url], started)) {
            _bridges.remove(url);
          }
        },
        onError: (Object _) {
          if (identical(_bridges[url], started)) {
            _bridges.remove(url);
          }
        },
      ),
    );
    return started;
  }

  /// Stops every bridge. Product code lets [ensure] retire a stale child on
  /// the next request; tests use this to leave no process behind.
  static Future<void> stopAll() async {
    final List<Future<ServerProxyBridge>> bridges = _bridges.values.toList(
      growable: false,
    );
    _bridges.clear();
    for (final Future<ServerProxyBridge> bridge in bridges) {
      try {
        await (await bridge).stop();
      } catch (_) {
        // A bridge that never started has no process to stop.
      }
    }
  }

  static Future<ServerProxyBridge> _start(
    String url,
    String executable,
    Duration timeout,
  ) async {
    final Process process;
    try {
      process = await Process.start(executable, _arguments, runInShell: false);
    } on ProcessException catch (error) {
      throw ServerProxyException(
        tr('无法启动代理桥接（{exe}）：{p2}', <String, Object?>{
          'exe': executable,
          'p2': error.message,
        }),
      );
    }
    final ServerProxyBridge bridge = ServerProxyBridge._(process, url);
    final Completer<int> ready = Completer<int>();
    bridge._stdout = process.stdout
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(
          (String line) => bridge._onLine(line, ready),
          onError: bridge._onStreamError,
        );
    bridge._stderr = process.stderr
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(bridge._noteLog, onError: bridge._onStreamError);
    unawaited(
      process.exitCode.then((int code) {
        if (!bridge._exited.isCompleted) {
          bridge._exited.complete();
        }
        if (!ready.isCompleted && !bridge._stopping) {
          ready.completeError(
            ServerProxyException(
              tr('代理桥接启动失败（退出码 {code}）{p7}', <String, Object?>{
                'code': code,
                'p7': bridge._logSuffix(),
              }),
            ),
          );
        }
      }),
    );
    try {
      // The pipe stays open as a liveness signal: when this process exits,
      // the helper sees EOF and stops instead of lingering on the machine.
      process.stdin.write('$url\n');
      await process.stdin.flush();
    } catch (_) {
      // The child exited before reading; its status carries the reason.
    }
    try {
      bridge._port = await ready.future.timeout(timeout);
    } on TimeoutException {
      await bridge.stop();
      throw ServerProxyException(
        tr('代理桥接在 {p3} 秒内没有就绪{p4}', <String, Object?>{
          'p3': timeout.inSeconds,
          'p4': bridge._logSuffix(),
        }),
      );
    } catch (_) {
      await bridge.stop();
      rethrow;
    }
    return bridge;
  }

  void _onLine(String line, Completer<int> ready) {
    final String text = line.trim();
    if (text.isEmpty) {
      return;
    }
    if (text.startsWith('{')) {
      try {
        final Object? decoded = jsonDecode(text);
        if (decoded is Map && decoded['event'] == 'ready') {
          final int? port = TunnelAgent.parseListenPort(
            decoded['listen']?.toString() ?? '',
          );
          if (!ready.isCompleted) {
            if (port == null || port <= 0 || port > 65535) {
              ready.completeError(
                ServerProxyException(
                  tr('代理桥接返回了无效监听地址：{p5}', <String, Object?>{
                    'p5': decoded['listen']?.toString() ?? '',
                  }),
                ),
              );
            } else {
              ready.complete(port);
            }
          }
          return;
        }
      } catch (_) {
        // Not an event line; keep it in the log below.
      }
    }
    _noteLog(text);
  }

  void _onStreamError(Object error) {
    _noteLog('$error');
  }

  void _noteLog(String line) {
    final String text = line.trim();
    if (text.isEmpty) {
      return;
    }
    _logs.add(text);
    if (_logs.length > 8) {
      _logs.removeAt(0);
    }
  }

  String _logSuffix() {
    return _logs.isEmpty ? '' : '\n${_logs.join('\n')}';
  }

  Future<void> stop() async {
    if (_stopping) {
      return;
    }
    _stopping = true;
    await _stdout?.cancel();
    await _stderr?.cancel();
    _stdout = null;
    _stderr = null;
    try {
      await _process.stdin.close();
    } catch (_) {
      // The child is already gone.
    }
    if (!_process.kill()) {
      // Already gone; exitCode still completes.
    }
    try {
      await _process.exitCode.timeout(const Duration(seconds: 2));
    } on TimeoutException {
      _process.kill(ProcessSignal.sigkill);
    }
  }
}
