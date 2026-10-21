import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'server_settings.dart';

/// One JSON line printed by `chast-tunnel viewer`. Stdout carries only these
/// events so the client never has to parse human log text; diagnostics are on
/// stderr.
class TunnelEvent {
  const TunnelEvent({
    required this.event,
    this.listen = '',
    this.host = '',
    this.path = '',
    this.remote = '',
    this.direct = '',
    this.relay = '',
  });

  final String event;
  final String listen;
  final String host;
  final String path;
  final String remote;
  final String direct;
  final String relay;

  static TunnelEvent? decode(String line) {
    final String text = line.trim();
    if (!text.startsWith('{')) {
      return null;
    }
    try {
      final Object? value = jsonDecode(text);
      if (value is! Map) {
        return null;
      }
      String field(String key) => value[key]?.toString() ?? '';
      final String event = field('event');
      if (event != 'ready' && event != 'path') {
        return null;
      }
      return TunnelEvent(
        event: event,
        listen: field('listen'),
        host: field('host'),
        path: field('path'),
        remote: field('remote'),
        direct: field('direct'),
        relay: field('relay'),
      );
    } catch (_) {
      return null;
    }
  }
}

class TunnelException implements Exception {
  const TunnelException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Owns one `chast-tunnel viewer` child process for a session.
///
/// The agent listens on a loopback UDP port chosen by the OS and forwards
/// every datagram to the direct candidate or relay. ViewerEngine therefore
/// dials 127.0.0.1; certificate pinning still uses the logical host identity
/// in [ConnectTarget.trustKey].
class TunnelAgent {
  TunnelAgent._(this._process, this._executable);

  static const String loopback = '127.0.0.1';

  final Process _process;
  final String _executable;
  StreamSubscription<String>? _stdout;
  StreamSubscription<String>? _stderr;
  final List<String> _logs = <String>[];
  Completer<void>? _ready;
  String _path = 'connecting';
  String _remote = '';
  int _port = 0;
  bool _running = true;
  bool _stopping = false;

  static String defaultExecutable() {
    final String? override = Platform.environment['CHAST_TUNNEL'];
    if (override != null && override.trim().isNotEmpty) {
      return override.trim();
    }
    final String dir = File(Platform.resolvedExecutable).parent.path;
    final String bundled = Platform.isWindows
        ? '$dir\\chast-tunnel.exe'
        : '$dir/chast-tunnel';
    if (File(bundled).existsSync()) {
      return bundled;
    }
    return Platform.isWindows ? 'chast-tunnel.exe' : 'chast-tunnel';
  }

  static List<String> arguments({
    required String server,
    required String hostId,
    required TunnelMode mode,
  }) {
    return <String>[
      'viewer',
      '--server',
      server,
      '--id',
      hostId,
      '--listen',
      '$loopback:0',
      '--mode',
      mode.name,
    ];
  }

  /// Starts the agent and waits for its ready event. [executable] and
  /// [argumentOverride] are test seams; product code uses the defaults.
  static Future<TunnelAgent> start({
    required ServerSettings server,
    required String hostId,
    String? executable,
    List<String>? argumentOverride,
    Duration timeout = const Duration(seconds: 15),
  }) async {
    if (!server.configured) {
      throw const TunnelException('未配置连接服务');
    }
    final String exe = executable ?? defaultExecutable();
    final List<String> args = argumentOverride ??
        arguments(server: server.url, hostId: hostId, mode: server.mode);
    final Process process;
    try {
      process = await Process.start(exe, args, runInShell: false);
    } on ProcessException catch (error) {
      throw TunnelException(
        '无法启动隧道代理（$exe）：${error.message}\n'
        '请确认 chast-tunnel.exe 与会话程序位于同一目录。',
      );
    }
    final TunnelAgent agent = TunnelAgent._(process, exe);
    agent._ready = Completer<void>();
    agent._stdout = process.stdout
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(agent._onLine, onError: agent._onStreamError);
    agent._stderr = process.stderr
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(agent._noteLog, onError: agent._onStreamError);
    unawaited(agent._watchExit());
    try {
      await agent._ready!.future.timeout(timeout);
    } on TimeoutException {
      await agent.stop();
      throw TunnelException(
        '隧道代理在 ${timeout.inSeconds} 秒内没有就绪'
        '${agent._logSuffix()}',
      );
    } catch (_) {
      await agent.stop();
      rethrow;
    }
    return agent;
  }

  String get host => loopback;
  int get port => _port;
  String get path => _path;
  String get remote => _remote;
  bool get running => _running;
  String get executable => _executable;

  /// "直连"/"中继"/"连接中" for the session overlay.
  String get pathLabel {
    switch (_path) {
      case 'direct':
        return '直连';
      case 'relay':
        return '中继';
      default:
        return '连接中';
    }
  }

  void _onLine(String line) {
    final TunnelEvent? event = TunnelEvent.decode(line);
    if (event == null) {
      if (line.trim().isNotEmpty) {
        _noteLog(line);
      }
      return;
    }
    if (event.path.isNotEmpty) {
      _path = event.path;
    }
    if (event.remote.isNotEmpty) {
      _remote = event.remote;
    }
    if (event.event == 'ready') {
      final int? port = parseListenPort(event.listen);
      if (port == null || port <= 0 || port > 65535) {
        _fail(TunnelException('隧道代理返回了无效监听地址：${event.listen}'));
        return;
      }
      _port = port;
      if (!(_ready?.isCompleted ?? true)) {
        _ready!.complete();
      }
    }
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

  Future<void> _watchExit() async {
    final int code = await _process.exitCode;
    _running = false;
    if (!_stopping && !(_ready?.isCompleted ?? true)) {
      _fail(TunnelException(
        '隧道代理启动失败（退出码 $code）${_logSuffix()}',
      ));
    }
  }

  void _fail(Object error) {
    if (!(_ready?.isCompleted ?? true)) {
      _ready!.completeError(error);
    }
  }

  Future<void> stop() async {
    if (_stopping) {
      return;
    }
    _stopping = true;
    _running = false;
    await _stdout?.cancel();
    await _stderr?.cancel();
    _stdout = null;
    _stderr = null;
    if (!_process.kill()) {
      // Already gone; exitCode still completes.
    }
    try {
      await _process.exitCode.timeout(const Duration(seconds: 2));
    } on TimeoutException {
      _process.kill(ProcessSignal.sigkill);
    }
  }

  /// "127.0.0.1:41234" or "[::1]:41234" -> port, or null.
  static int? parseListenPort(String listen) {
    final String text = listen.trim();
    if (text.isEmpty) {
      return null;
    }
    final int colon = text.lastIndexOf(':');
    if (colon < 0 || colon + 1 >= text.length) {
      return null;
    }
    return int.tryParse(text.substring(colon + 1));
  }
}
