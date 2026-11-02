import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'l10n.dart';

/// A parsed SOCKS5 endpoint, `socks5://[user:pass@]host:port`.
///
/// The viewer routes the service HTTP API and the TCP carrier through it.
/// QUIC and every UDP path stay direct: SOCKS5 cannot carry datagrams, and a
/// silent UDP fallback would expose the address the proxy exists to hide.
class Socks5Proxy {
  const Socks5Proxy({
    required this.host,
    required this.port,
    this.username = '',
    this.password = '',
  });

  final String host;
  final int port;
  final String username;
  final String password;

  bool get hasCredentials => username.isNotEmpty;

  /// Canonical `socks5://` URL without the password, for logs and status text.
  String get display {
    final String name = host.contains(':') ? '[$host]' : host;
    final String user = username.isEmpty ? '' : '$username@';
    return 'socks5://$user$name:$port';
  }

  static final RegExp _scheme = RegExp(
    r'^socks5h?://',
    caseSensitive: false,
  );
  static final RegExp _forbidden = RegExp(r'[\s/?#]');
  static final RegExp _host = RegExp(r'^[A-Za-z0-9._-]+$');
  static final RegExp _ipv6 = RegExp(r'^[0-9A-Fa-f:.%]+$');

  /// Returns '' for an empty setting, the canonical URL for a valid one, and
  /// null when [input] cannot be used.
  static String? normalize(String input) {
    final String text = input.trim();
    if (text.isEmpty) {
      return '';
    }
    return tryParse(text)?.url;
  }

  static Socks5Proxy? tryParse(String input) {
    String text = input.trim();
    if (text.isEmpty || text.length > 512) {
      return null;
    }
    bool scheme = false;
    final RegExpMatch? match = _scheme.firstMatch(text);
    if (match != null) {
      scheme = true;
      text = text.substring(match.end);
    }
    if (text.isEmpty || text.contains(_forbidden)) {
      return null;
    }
    String username = '';
    String password = '';
    String authority = text;
    final int at = text.lastIndexOf('@');
    if (at >= 0) {
      if (!scheme) {
        return null;
      }
      final String userInfo = text.substring(0, at);
      authority = text.substring(at + 1);
      final int colon = userInfo.indexOf(':');
      username = colon < 0 ? userInfo : userInfo.substring(0, colon);
      password = colon < 0 ? '' : userInfo.substring(colon + 1);
      if (username.isEmpty ||
          username.length > 255 ||
          password.length > 255 ||
          !_safeCredentials(username) ||
          !_safeCredentials(password)) {
        return null;
      }
    }
    String host;
    int port;
    if (authority.startsWith('[')) {
      final int end = authority.indexOf(']');
      if (end <= 1) {
        return null;
      }
      host = authority.substring(1, end);
      final String rest = authority.substring(end + 1);
      if (rest.isEmpty) {
        if (!scheme) {
          return null;
        }
        port = 1080;
      } else {
        if (!rest.startsWith(':')) {
          return null;
        }
        port = _port(rest.substring(1));
        if (port == 0) {
          return null;
        }
      }
      if (!_ipv6.hasMatch(host)) {
        return null;
      }
    } else {
      final int colon = authority.lastIndexOf(':');
      if (colon < 0) {
        if (!scheme) {
          return null;
        }
        host = authority;
        port = 1080;
      } else {
        if (authority.indexOf(':') != colon) {
          return null;
        }
        host = authority.substring(0, colon);
        port = _port(authority.substring(colon + 1));
        if (port == 0) {
          return null;
        }
      }
      if (!_host.hasMatch(host)) {
        return null;
      }
    }
    return Socks5Proxy(
      host: host,
      port: port,
      username: username,
      password: password,
    );
  }

  /// Canonical setting value; credentials are stored verbatim because the
  /// parser only accepts characters that survive a round trip.
  String get url {
    final String credentials = username.isEmpty ? '' : '$username:$password@';
    final String name = host.contains(':') ? '[$host]' : host;
    return 'socks5://$credentials$name:$port';
  }

  /// An [HttpClient] whose connections (including https) go through the proxy.
  /// Empty [setting] yields a plain client; an unusable one fails closed
  /// instead of silently connecting directly.
  static HttpClient httpClient(String setting) {
    final HttpClient client = HttpClient();
    final String text = setting.trim();
    if (text.isEmpty) {
      return client;
    }
    final Socks5Proxy? proxy = tryParse(text);
    if (proxy == null) {
      throw Socks5Exception(
        tr('SOCKS5 代理无效，需要 socks5://[用户名:密码@]主机:端口'),
      );
    }
    client.findProxy = (Uri uri) => 'DIRECT';
    client.connectionFactory = proxy._connectionFactory;
    return client;
  }

  Future<ConnectionTask<Socket>> _connectionFactory(
    Uri target,
    String? proxyHost,
    int? proxyPort,
  ) async {
    return ConnectionTask.fromSocket(_open(target), () {});
  }

  Future<Socket> _open(Uri target) async {
    final InternetAddress? literal = InternetAddress.tryParse(host);
    final Socket raw = literal != null
        ? await Socket.connect(literal, port)
        : await Socket.connect(host, port);
    final _TunnelSocket tunnel = _TunnelSocket(raw);
    try {
      await _negotiate(tunnel, target);
      if (target.isScheme('https')) {
        if (tunnel.hasBufferedData) {
          throw Socks5Exception(tr('SOCKS5 代理返回了无效响应'));
        }
        final SecureSocket secure =
            await SecureSocket.secure(raw, host: target.host);
        await tunnel.detach();
        return secure;
      }
      tunnel.finishHandshake();
      return tunnel;
    } catch (_) {
      tunnel.destroy();
      rethrow;
    }
  }

  Future<void> _negotiate(_TunnelSocket socket, Uri target) async {
    final List<int> methods = <int>[0x00, if (hasCredentials) 0x02];
    socket.add(<int>[0x05, methods.length, ...methods]);
    await socket.flush();
    final Uint8List choice = await socket.read(2);
    if (choice[0] != 0x05) {
      throw Socks5Exception(tr('SOCKS5 代理返回了无效响应'));
    }
    if (choice[1] == 0xFF) {
      throw Socks5Exception(tr('SOCKS5 代理不接受可用的认证方式'));
    }
    if (choice[1] == 0x02) {
      if (!hasCredentials) {
        throw Socks5Exception(tr('SOCKS5 代理要求用户名和密码'));
      }
      final List<int> user = username.codeUnits;
      final List<int> pass = password.codeUnits;
      socket.add(<int>[0x01, user.length, ...user, pass.length, ...pass]);
      await socket.flush();
      final Uint8List auth = await socket.read(2);
      if (auth[0] != 0x01 || auth[1] != 0x00) {
        throw Socks5Exception(tr('SOCKS5 代理认证失败'));
      }
    } else if (choice[1] != 0x00) {
      throw Socks5Exception(tr('SOCKS5 代理返回了无效响应'));
    }
    socket.add(_connectRequest(target));
    await socket.flush();
    final Uint8List head = await socket.read(4);
    if (head[0] != 0x05) {
      throw Socks5Exception(tr('SOCKS5 代理返回了无效响应'));
    }
    if (head[1] != 0x00) {
      throw Socks5Exception(
        tr('SOCKS5 代理拒绝了连接（代码 {p63}）', <String, Object?>{
          'p63': head[1],
        }),
      );
    }
    if (head[3] == 0x01) {
      await socket.read(4);
    } else if (head[3] == 0x04) {
      await socket.read(16);
    } else if (head[3] == 0x03) {
      final Uint8List length = await socket.read(1);
      await socket.read(length[0]);
    } else {
      throw Socks5Exception(tr('SOCKS5 代理返回了无效响应'));
    }
    await socket.read(2);
  }

  List<int> _connectRequest(Uri target) {
    final InternetAddress? literal = InternetAddress.tryParse(target.host);
    final List<int> address;
    if (literal == null) {
      final List<int> name = utf8.encode(target.host);
      address = <int>[0x03, name.length, ...name];
    } else if (literal.type == InternetAddressType.IPv6) {
      address = <int>[0x04, ...literal.rawAddress];
    } else {
      address = <int>[0x01, ...literal.rawAddress];
    }
    return <int>[
      0x05,
      0x01,
      0x00,
      ...address,
      target.port >> 8 & 0xff,
      target.port & 0xff,
    ];
  }

  static bool _safeCredentials(String text) {
    for (final int unit in text.codeUnits) {
      if (unit < 0x21 ||
          unit > 0x7e ||
          unit == 0x3a ||
          unit == 0x40 ||
          unit == 0x2f ||
          unit == 0x3f ||
          unit == 0x23) {
        return false;
      }
    }
    return true;
  }

  static int _port(String text) {
    if (text.isEmpty || text.length > 5) {
      return 0;
    }
    for (final int unit in text.codeUnits) {
      if (unit < 0x30 || unit > 0x39) {
        return 0;
      }
    }
    final int value = int.parse(text);
    return value <= 0 || value > 65535 ? 0 : value;
  }
}

class Socks5Exception implements Exception {
  const Socks5Exception(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Owns the proxied connection for its whole life.
///
/// A socket stream can only be listened to once, and cancelling the handshake
/// read would shut the receive side down. This wrapper keeps a single
/// subscription, buffers bytes in [read] while the SOCKS5 handshake runs, and
/// then exposes the same stream to [HttpClient] with backpressure forwarding.
/// TLS hands the raw socket to [SecureSocket.secure] and detaches instead.
class _TunnelSocket extends Stream<Uint8List> implements Socket {
  _TunnelSocket(this._socket) {
    _source = _socket.listen(
      _receive,
      onError: _onError,
      onDone: _onDone,
    );
    _output = StreamController<Uint8List>(
      sync: true,
      onPause: () {
        if (!_source.isPaused) {
          _source.pause();
        }
      },
      onResume: () {
        if (_source.isPaused) {
          _source.resume();
        }
      },
      onCancel: () {
        unawaited(_source.cancel());
      },
    );
  }

  final Socket _socket;
  late final StreamSubscription<Uint8List> _source;
  late final StreamController<Uint8List> _output;
  final List<Uint8List> _pending = <Uint8List>[];
  Completer<void>? _wake;
  bool _handshaking = true;
  bool _listening = false;
  bool _closed = false;
  Object? _failure;
  StackTrace? _stackTrace;

  bool get hasBufferedData => _pending.isNotEmpty;

  void finishHandshake() {
    _handshaking = false;
  }

  Future<void> detach() async {
    _handshaking = false;
    _listening = true;
    await _source.cancel();
  }

  /// Exclusive read for the SOCKS5 handshake; leftover bytes are kept for
  /// [listen], where the protocol payload starts.
  Future<Uint8List> read(int count) async {
    final BytesBuilder builder = BytesBuilder(copy: false);
    while (builder.length < count) {
      if (_pending.isEmpty) {
        if (_failure != null || _closed) {
          throw Socks5Exception(tr('SOCKS5 代理提前关闭了连接'));
        }
        _wake = Completer<void>();
        await _wake!.future;
        _wake = null;
        continue;
      }
      final Uint8List chunk = _pending.first;
      final int need = count - builder.length;
      if (chunk.length <= need) {
        _pending.removeAt(0);
        builder.add(chunk);
      } else {
        builder.add(Uint8List.sublistView(chunk, 0, need));
        _pending[0] = Uint8List.sublistView(chunk, need);
      }
    }
    return builder.takeBytes();
  }

  void _receive(Uint8List chunk) {
    if (_handshaking || !_listening) {
      _pending.add(chunk);
    } else {
      _output.add(chunk);
    }
    _wakeUp();
  }

  void _onError(Object error, StackTrace stackTrace) {
    _failure = error;
    _stackTrace = stackTrace;
    if (!_handshaking && _listening) {
      _output.addError(error, stackTrace);
      _failure = null;
      _stackTrace = null;
    }
    _wakeUp();
  }

  void _onDone() {
    _closed = true;
    if (!_handshaking && _listening && !_output.isClosed) {
      unawaited(_output.close());
    }
    _wakeUp();
  }

  void _wakeUp() {
    final Completer<void>? wake = _wake;
    if (wake != null && !wake.isCompleted) {
      wake.complete();
    }
  }

  @override
  StreamSubscription<Uint8List> listen(
    void Function(Uint8List event)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) {
    _listening = true;
    for (final Uint8List chunk in _pending) {
      _output.add(chunk);
    }
    _pending.clear();
    final Object? failure = _failure;
    if (failure != null) {
      _output.addError(failure, _stackTrace);
      _failure = null;
      _stackTrace = null;
    }
    if (_closed && !_output.isClosed) {
      unawaited(_output.close());
    }
    return _output.stream.listen(
      onData,
      onError: onError,
      onDone: onDone,
      cancelOnError: cancelOnError,
    );
  }

  @override
  void add(List<int> data) => _socket.add(data);

  @override
  void addError(Object error, [StackTrace? stackTrace]) =>
      _socket.addError(error, stackTrace);

  @override
  Future<void> addStream(Stream<List<int>> stream) => _socket.addStream(stream);

  @override
  Future<void> close() => _socket.close();

  @override
  Future<void> get done => _socket.done;

  @override
  Future<void> flush() => _socket.flush();

  @override
  void write(Object? object) => _socket.write(object);

  @override
  void writeAll(Iterable<Object?> objects, [String separator = '']) =>
      _socket.writeAll(objects, separator);

  @override
  void writeCharCode(int charCode) => _socket.writeCharCode(charCode);

  @override
  void writeln([Object? object = '']) => _socket.writeln(object);

  @override
  Encoding get encoding => _socket.encoding;

  @override
  set encoding(Encoding value) => _socket.encoding = value;

  @override
  InternetAddress get address => _socket.address;

  @override
  int get port => _socket.port;

  @override
  InternetAddress get remoteAddress => _socket.remoteAddress;

  @override
  int get remotePort => _socket.remotePort;

  @override
  bool setOption(SocketOption option, bool enabled) =>
      _socket.setOption(option, enabled);

  @override
  Uint8List getRawOption(RawSocketOption option) =>
      _socket.getRawOption(option);

  @override
  void setRawOption(RawSocketOption option) =>
      _socket.setRawOption(option);

  @override
  void destroy() {
    _socket.destroy();
    if (!_output.isClosed) {
      unawaited(_output.close());
    }
  }
}
