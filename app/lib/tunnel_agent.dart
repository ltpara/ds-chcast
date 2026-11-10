import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'server_settings.dart';
import 'package:chast_viewer/l10n.dart';

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
    this.carrier = '',
    this.carrierToken = '',
    this.accessTicket = '',
    this.accessExpiresAtMs = 0,
    this.accessSubject = '',
    this.accessPermissions = 0,
    this.error = '',
    this.retryable = false,
  });

  final String event;
  final String listen;
  final String host;
  final String path;
  final String remote;
  final String direct;
  final String relay;
  final String carrier;
  final String carrierToken;

  /// Emergency override ticket. The initial one rides on the ready event; the
  /// agent re-signs the override whenever it refreshes a transport credential
  /// and sends the replacement as an access_ticket event.
  final String accessTicket;
  final int accessExpiresAtMs;
  final String accessSubject;
  final int accessPermissions;
  final String error;
  final bool retryable;

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
      int number(String key) {
        final Object? raw = value[key];
        return raw is int ? raw : int.tryParse(raw?.toString() ?? '') ?? 0;
      }

      final String event = field('event');
      if (event != 'ready' &&
          event != 'path' &&
          event != 'error' &&
          event != 'access_ticket') {
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
        carrier: field('carrier'),
        carrierToken: field('carrier_token'),
        accessTicket: field('access_ticket'),
        accessExpiresAtMs: number('access_expires_at_ms'),
        accessSubject: field('access_subject'),
        accessPermissions: number('access_permissions'),
        error: field('error'),
        retryable: value['retryable'] == true,
      );
    } catch (_) {
      return null;
    }
  }
}

class TunnelException implements Exception {
  const TunnelException(this.message, {this.retryable = false});

  final String message;
  final bool retryable;

  @override
  String toString() => message;
}

/// An explicit management override for a restricted device. The administrator
/// token is passed over stdin, never argv, and is kept only in memory by the
/// page that collects it.
class EmergencyAccessRequest {
  const EmergencyAccessRequest({
    required this.adminToken,
    required this.reason,
    required this.clientPublicKey,
    this.permissions = 0,
  });

  final String adminToken;
  final String reason;

  /// base64url Ed25519 public key of the application client the access ticket
  /// is bound to. The tunnel contributes its own ephemeral transport key.
  final String clientPublicKey;

  /// Permission mask to request; 0 asks for the device's full ceiling.
  final int permissions;

  bool get ready =>
      adminToken.isNotEmpty && reason.isNotEmpty && clientPublicKey.isNotEmpty;
}

/// One emergency access ticket delivered by the tunnel agent, either in its
/// ready event or after the agent re-signed the override.
class TunnelAccessTicket {
  const TunnelAccessTicket({
    required this.ticket,
    required this.expiresAtMs,
    this.subject = '',
    this.permissions = 0,
  });

  final String ticket;
  final int expiresAtMs;
  final String subject;
  final int permissions;
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
  final Completer<void> _stdoutDone = Completer<void>();
  final Completer<void> _stderrDone = Completer<void>();
  String _path = 'connecting';
  String _remote = '';
  String _relay = '';
  String _carrier = '';
  String _carrierToken = '';
  String _accessTicket = '';
  int _accessExpiresAtMs = 0;
  final StreamController<TunnelAccessTicket> _ticketEvents =
      StreamController<TunnelAccessTicket>.broadcast();
  int _port = 0;
  bool _running = true;
  bool _stopping = false;

  /// Hands a verified viewer release to the bundled helper. The helper waits
  /// for this process to exit, swaps the executable and starts the new one;
  /// the caller must exit right after this returns.
  static Future<void> applyUpdate({
    required String version,
    required String url,
    required String sha256,
    required int size,
    required String signature,
    required String serverPublicKey,
    String publisherSignature = '',
    String? executable,
  }) async {
    if (!Platform.isWindows) {
      throw UnsupportedError('automatic update is only supported on Windows');
    }
    final String helper = executable ?? defaultExecutable();
    // The publisher key is a second, independent pin. Deployments that hold
    // the publisher public key set CHAST_PUBLISHER_KEY; without it the helper
    // still verifies the server signature.
    final String publisherKey =
        (Platform.environment['CHAST_PUBLISHER_KEY'] ?? '').trim();
    final bool requirePublisher =
        (Platform.environment['CHAST_REQUIRE_PUBLISHER'] ?? '') == '1';
    await Process.start(
      helper,
      <String>[
        'apply-update',
        '--version',
        version,
        '--url',
        url,
        '--sha256',
        sha256,
        '--size',
        '$size',
        '--signature',
        signature,
        '--server-pubkey',
        serverPublicKey,
        if (publisherKey.isNotEmpty) ...<String>[
          '--publisher-pubkey',
          publisherKey,
          '--publisher-sig',
          publisherSignature,
        ],
        if (requirePublisher) '--require-publisher',
        '--target',
        Platform.resolvedExecutable,
        '--wait-pid',
        '$pid',
        '--log',
        '${Directory.systemTemp.path}\\chast-viewer-update.log',
      ],
      mode: ProcessStartMode.detached,
    );
  }

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
    String key = '',
    String linkKey = '',
    String accountToken = '',
    String serverPublicKey = '',
    String proxy = '',
    EmergencyAccessRequest? emergency,
  }) {
    final bool emergencyReady = emergency?.ready ?? false;
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
      // The keys, the account token and the proxy URL (which may carry
      // credentials) travel over stdin, never argv: any process of this user,
      // EDR and audit logs can read a child's command line. The link key
      // replaces the management key when the deployment issues one.
      if (linkKey.isNotEmpty)
        '--link-key-stdin'
      else if (key.isNotEmpty)
        '--server-key-stdin',
      if (accountToken.isNotEmpty) '--account-token-stdin',
      if (emergencyReady) ...<String>[
        '--emergency-token-stdin',
        '--emergency-reason-stdin',
        '--emergency-client-key',
        emergency!.clientPublicKey,
        if (emergency.permissions != 0) ...<String>[
          '--emergency-permissions',
          '${emergency.permissions}',
        ],
      ],
      if (proxy.isNotEmpty) '--proxy-stdin',
      if (serverPublicKey.isNotEmpty) ...<String>[
        '--server-pubkey',
        serverPublicKey,
      ],
    ];
  }

  /// Starts the agent and waits for its ready event. [executable] and
  /// [argumentOverride] are test seams; product code uses the defaults.
  static Future<TunnelAgent> start({
    required ServerSettings server,
    required String hostId,
    String accountToken = '',
    String serverPublicKey = '',
    EmergencyAccessRequest? emergency,
    String? executable,
    List<String>? argumentOverride,
    Duration timeout = const Duration(seconds: 15),
  }) async {
    if (!server.configured) {
      throw TunnelException(tr('未配置连接服务'));
    }
    final String exe = executable ?? defaultExecutable();
    final List<String> args = argumentOverride ??
        arguments(
          server: server.url,
          hostId: hostId,
          mode: server.mode,
          key: server.key,
          linkKey: server.linkKey,
          accountToken: accountToken,
          serverPublicKey: serverPublicKey,
          proxy: server.proxy,
          emergency: emergency,
        );
    final Process process;
    try {
      process = await Process.start(exe, args, runInShell: false);
    } on ProcessException catch (error) {
      throw TunnelException(
        tr('无法启动隧道代理（{exe}）：{p2}\n请确认 chast-tunnel.exe 与会话程序位于同一目录。', <String, Object?>{'exe': exe, 'p2': error.message}),
      );
    }
    final String credential =
        server.linkKey.isNotEmpty ? server.linkKey : server.key;
    final bool emergencyReady = emergency?.ready ?? false;
    if (credential.isNotEmpty ||
        accountToken.isNotEmpty ||
        emergencyReady ||
        server.proxy.isNotEmpty) {
      try {
        // Order matches the helper: connection credential, account token,
        // emergency administrator token and reason, then proxy. The account
        // token authenticates lookup as the signed-in user; the connection key
        // stays the legacy punch fallback. The emergency lines are read only
        // when their flags are present.
        if (credential.isNotEmpty) {
          process.stdin.write('$credential\n');
        }
        if (accountToken.isNotEmpty) {
          process.stdin.write('$accountToken\n');
        }
        if (emergencyReady) {
          process.stdin.write('${emergency!.adminToken}\n');
          process.stdin.write('${emergency.reason}\n');
        }
        if (server.proxy.isNotEmpty) {
          process.stdin.write('${server.proxy}\n');
        }
        await process.stdin.close();
      } catch (_) {
        // The child exited before reading; its exit status carries the error.
      }
    }
    final TunnelAgent agent = TunnelAgent._(process, exe);
    agent._ready = Completer<void>();
    agent._stdout = process.stdout
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(agent._onLine,
            onError: agent._onStreamError, onDone: agent._stdoutDone.complete);
    agent._stderr = process.stderr
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(agent._noteLog,
            onError: agent._onStreamError, onDone: agent._stderrDone.complete);
    unawaited(agent._watchExit());
    try {
      await agent._ready!.future.timeout(timeout);
    } on TimeoutException {
      await agent.stop();
      throw TunnelException(
        tr('隧道代理在 {p3} 秒内没有就绪{p4}', <String, Object?>{'p3': timeout.inSeconds, 'p4': agent._logSuffix()}),
        retryable: true,
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

  /// The relay address the server assigned to this host, "ip:port"; empty
  /// until a ready/path event carried one. The service listens for carrier
  /// TCP and WebSocket on the same numeric port as UDP, so this is the
  /// fallback endpoint when the direct host port is unreachable.
  String get relay => _relay;

  /// Host part of [relay], without the brackets of an IPv6 literal.
  String get relayHost {
    final String text = _relay.trim();
    if (text.startsWith('[')) {
      final int end = text.indexOf(']');
      return end > 1 ? text.substring(1, end) : '';
    }
    final int colon = text.lastIndexOf(':');
    return colon > 0 ? text.substring(0, colon) : '';
  }

  /// Port part of [relay], or 0 when no relay arrived.
  int get relayPort => parseListenPort(_relay) ?? 0;

  /// Shared TCP/WebSocket carrier the server advertised for this host,
  /// "ip:port"; empty when the server runs without one.
  String get carrier => _carrier;

  /// Host part of [carrier], without the brackets of an IPv6 literal.
  String get carrierHost {
    final String text = _carrier.trim();
    if (text.startsWith('[')) {
      final int end = text.indexOf(']');
      return end > 1 ? text.substring(1, end) : '';
    }
    final int colon = text.lastIndexOf(':');
    return colon > 0 ? text.substring(0, colon) : '';
  }

  /// Port part of [carrier], or 0 when the server has no shared carrier.
  int get carrierPort => parseListenPort(_carrier) ?? 0;

  /// Per-lease token that lets this viewer attach to its host slot on the
  /// shared carrier. Empty when the server has no shared carrier.
  String get carrierToken => _carrierToken;

  /// The current emergency override ticket, or empty on an ordinary lookup.
  /// The page hands this to the native session on start.
  String get accessTicket => _accessTicket;
  int get accessExpiresAtMs => _accessExpiresAtMs;

  /// Renewed emergency tickets. The agent re-signs the override whenever it
  /// refreshes a transport credential, and the live session must follow it or
  /// the host lease expires mid-session.
  Stream<TunnelAccessTicket> get accessTickets => _ticketEvents.stream;

  /// "直连"/"中继"/"连接中" for the session overlay.
  String get pathLabel {
    switch (_path) {
      case 'direct':
        return tr('直连');
      case 'relay':
        return tr('中继');
      default:
        return tr('连接中');
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
    if (event.event == 'error') {
      _fail(TunnelException(event.error, retryable: event.retryable));
      return;
    }
    if (event.accessTicket.isNotEmpty) {
      _accessTicket = event.accessTicket;
      _accessExpiresAtMs = event.accessExpiresAtMs;
      if (!_ticketEvents.isClosed) {
        _ticketEvents.add(TunnelAccessTicket(
          ticket: event.accessTicket,
          expiresAtMs: event.accessExpiresAtMs,
          subject: event.accessSubject,
          permissions: event.accessPermissions,
        ));
      }
    }
    if (event.path.isNotEmpty) {
      _path = event.path;
    }
    if (event.remote.isNotEmpty) {
      _remote = event.remote;
    }
    if (event.relay.isNotEmpty) {
      _relay = event.relay;
    }
    if (event.carrier.isNotEmpty) {
      _carrier = event.carrier;
    }
    if (event.carrierToken.isNotEmpty) {
      _carrierToken = event.carrierToken;
    }
    if (event.event == 'ready') {
      final int? port = parseListenPort(event.listen);
      if (port == null || port <= 0 || port > 65535) {
        _fail(TunnelException(tr('隧道代理返回了无效监听地址：{p5}', <String, Object?>{'p5': event.listen})));
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
    // Process exit can arrive before its final JSON error or diagnostics.
    await Future.wait(<Future<void>>[_stdoutDone.future, _stderrDone.future]);
    if (!_stopping && !(_ready?.isCompleted ?? true)) {
      _fail(TunnelException(
        tr('隧道代理启动失败（退出码 {code}）{p7}', <String, Object?>{'code': code, 'p7': _logSuffix()}),
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
    if (!_stdoutDone.isCompleted) _stdoutDone.complete();
    if (!_stderrDone.isCompleted) _stderrDone.complete();
    _stdout = null;
    _stderr = null;
    if (!_ticketEvents.isClosed) {
      await _ticketEvents.close();
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
