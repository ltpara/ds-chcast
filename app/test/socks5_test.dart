import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/socks5.dart';

void main() {
  group('parse', () {
    test('accepts scheme, bare and authenticated forms', () {
      expect(Socks5Proxy.normalize(''), '');
      expect(Socks5Proxy.normalize('   '), '');
      expect(
        Socks5Proxy.normalize('socks5://127.0.0.1:1080'),
        'socks5://127.0.0.1:1080',
      );
      expect(
        Socks5Proxy.normalize('socks5h://proxy.example'),
        'socks5://proxy.example:1080',
      );
      expect(
        Socks5Proxy.normalize('socks5://alice:s3cret@127.0.0.1:9050'),
        'socks5://alice:s3cret@127.0.0.1:9050',
      );
      expect(
        Socks5Proxy.normalize('127.0.0.1:9050'),
        'socks5://127.0.0.1:9050',
      );
      expect(Socks5Proxy.normalize('[::1]:1080'), 'socks5://[::1]:1080');
      expect(
        Socks5Proxy.normalize('socks5://[2001:db8::1]'),
        'socks5://[2001:db8::1]:1080',
      );
      expect(
        Socks5Proxy.normalize('socks5://alice@127.0.0.1:1080'),
        'socks5://alice:@127.0.0.1:1080',
      );
      expect(
        Socks5Proxy.normalize('socks5://user:pass@127.0.0.1'),
        'socks5://user:pass@127.0.0.1:1080',
      );
    });

    test('rejects unusable forms', () {
      for (final String value in <String>[
        'http://127.0.0.1:1080',
        'socks4://127.0.0.1:1080',
        '127.0.0.1',
        'socks5://',
        'socks5://:1080',
        'socks5://host:0',
        'socks5://host:70000',
        'socks5://host:1080/path',
        'socks5://host:1080?x=1',
        'socks5://host:1080#top',
        'user:pass@127.0.0.1:1080',
        'socks5://bad host:1080',
        'socks5://user:pa ss@127.0.0.1:1080',
      ]) {
        expect(Socks5Proxy.normalize(value), isNull, reason: value);
      }
    });

    test('display omits the password', () {
      final Socks5Proxy proxy = Socks5Proxy.tryParse(
        'socks5://alice:s3cret@127.0.0.1:1080',
      )!;
      expect(proxy.display, 'socks5://alice@127.0.0.1:1080');
      expect(proxy.url, 'socks5://alice:s3cret@127.0.0.1:1080');
    });
  });

  group('http client', () {
    test('resolves the target name on the proxy', () async {
      final _FakeSocks5 proxy = _FakeSocks5();
      await proxy.start();
      addTearDown(proxy.close);

      final HttpClient client = Socks5Proxy.httpClient(
        'socks5://127.0.0.1:${proxy.port}',
      );
      addTearDown(() => client.close(force: true));

      final HttpClientResponse response = await (await client.getUrl(
        Uri.parse('http://chast.invalid:8080/v1/hosts'),
      )).close();
      expect(await utf8.decodeStream(response), 'hello');
      expect(proxy.targets, <String>['chast.invalid:8080']);
      expect(proxy.offered, contains(0x00));
    });

    test('sends the IPv4 literal as an address', () async {
      final _FakeSocks5 proxy = _FakeSocks5();
      await proxy.start();
      addTearDown(proxy.close);

      final HttpClient client = Socks5Proxy.httpClient(
        'socks5://127.0.0.1:${proxy.port}',
      );
      addTearDown(() => client.close(force: true));

      await (await client.getUrl(
        Uri.parse('http://127.0.0.1:8080/healthz'),
      )).close();
      expect(proxy.targets, <String>['127.0.0.1:8080']);
    });

    test('negotiates username and password authentication', () async {
      final _FakeSocks5 proxy = _FakeSocks5(
        username: 'alice',
        password: 's3cret',
      );
      await proxy.start();
      addTearDown(proxy.close);

      final HttpClient client = Socks5Proxy.httpClient(
        'socks5://alice:s3cret@127.0.0.1:${proxy.port}',
      );
      addTearDown(() => client.close(force: true));

      final HttpClientResponse response = await (await client.getUrl(
        Uri.parse('http://chast.invalid:8080/v1/server'),
      )).close();
      expect(await utf8.decodeStream(response), 'hello');
      expect(proxy.credentials, <String>['alice:s3cret']);
    });

    test('reports a refused CONNECT', () async {
      final _FakeSocks5 proxy = _FakeSocks5()..replyCode = 0x02;
      await proxy.start();
      addTearDown(proxy.close);

      final HttpClient client = Socks5Proxy.httpClient(
        'socks5://127.0.0.1:${proxy.port}',
      );
      addTearDown(() => client.close(force: true));

      Object? failure;
      try {
        await (await client.getUrl(
          Uri.parse('http://chast.invalid:8080/'),
        )).close();
      } catch (error) {
        failure = error;
      }
      expect(failure, isNotNull);
      expect('$failure', contains('SOCKS5'));
    });

    test('relays bytes to a real server after the handshake', () async {
      final HttpServer origin = await HttpServer.bind(
        InternetAddress.loopbackIPv4,
        0,
      );
      addTearDown(() => origin.close(force: true));
      origin.listen((HttpRequest request) {
        request.response.write('proxied');
        unawaited(request.response.close());
      });

      final _FakeSocks5 proxy = _FakeSocks5()..relay = true;
      await proxy.start();
      addTearDown(proxy.close);

      final HttpClient client = Socks5Proxy.httpClient(
        'socks5://127.0.0.1:${proxy.port}',
      );
      addTearDown(() => client.close(force: true));

      final HttpClientResponse response = await (await client.getUrl(
        Uri.parse('http://127.0.0.1:${origin.port}/healthz'),
      )).close();
      expect(await utf8.decodeStream(response), 'proxied');
      expect(proxy.targets, <String>['127.0.0.1:${origin.port}']);
    });

    test('an invalid stored setting fails closed', () {
      expect(
        () => Socks5Proxy.httpClient('http://127.0.0.1:8080'),
        throwsA(isA<Socks5Exception>()),
      );
    });
  });
}

/// Minimal SOCKS5 server: answers the handshake, then either relays to the
/// requested target or writes one canned HTTP response.
class _FakeSocks5 {
  _FakeSocks5({this.username = '', this.password = ''});

  final String username;
  final String password;
  bool relay = false;
  int? replyCode;

  final List<String> targets = <String>[];
  final List<String> credentials = <String>[];
  final List<int> offered = <int>[];

  ServerSocket? _server;
  final List<Socket> _clients = <Socket>[];

  int get port => _server!.port;

  Future<void> start() async {
    _server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    _server!.listen((Socket client) {
      _clients.add(client);
      unawaited(_serve(client));
    });
  }

  Future<void> close() async {
    for (final Socket client in _clients) {
      client.destroy();
    }
    _clients.clear();
    await _server?.close();
    _server = null;
  }

  Future<void> _serve(Socket client) async {
    final _Reader reader = _Reader(client);
    try {
      final Uint8List greeting = await reader.read(2);
      final int count = greeting[1];
      final Uint8List methods = await reader.read(count);
      offered.addAll(methods);
      final int method = username.isEmpty ? 0x00 : 0x02;
      if (!methods.contains(method)) {
        client.add(<int>[0x05, 0xff]);
        await client.flush();
        return;
      }
      client.add(<int>[0x05, method]);
      await client.flush();
      if (method == 0x02) {
        final Uint8List head = await reader.read(2);
        final String user = utf8.decode(await reader.read(head[1]));
        final Uint8List passLength = await reader.read(1);
        final String pass = utf8.decode(await reader.read(passLength[0]));
        credentials.add('$user:$pass');
        final bool ok = user == username && pass == password;
        client.add(<int>[0x01, ok ? 0x00 : 0x01]);
        await client.flush();
        if (!ok) {
          return;
        }
      }
      final Uint8List head = await reader.read(4);
      String host;
      if (head[3] == 0x01) {
        host = (await reader.read(4)).join('.');
      } else if (head[3] == 0x04) {
        host = InternetAddress.fromRawAddress(await reader.read(16)).address;
      } else {
        final Uint8List length = await reader.read(1);
        host = utf8.decode(await reader.read(length[0]));
      }
      final Uint8List portBytes = await reader.read(2);
      final int targetPort = portBytes[0] << 8 | portBytes[1];
      targets.add('$host:$targetPort');
      if (replyCode != null) {
        client.add(<int>[0x05, replyCode!, 0x00, 0x01, 0, 0, 0, 0, 0, 0]);
        await client.flush();
        return;
      }
      client.add(<int>[0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0, 0]);
      await client.flush();
      if (relay) {
        final Socket upstream = await Socket.connect(host, targetPort);
        upstream.listen(
          client.add,
          onDone: client.close,
          onError: (Object _) => client.destroy(),
        );
        await reader.pumpTo(upstream);
        await upstream.close();
        return;
      }
      // HTTP/TLS clients speak first; waiting for the request also keeps the
      // canned response from coalescing with the CONNECT reply.
      await reader.read(1);
      client.add(
        ascii.encode(
          'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n'
          'Connection: close\r\n\r\nhello',
        ),
      );
      await client.flush();
      await client.close();
    } catch (_) {
      client.destroy();
    } finally {
      await reader.close();
    }
  }
}

class _Reader {
  _Reader(Socket socket) : _iterator = StreamIterator<Uint8List>(socket);

  final StreamIterator<Uint8List> _iterator;
  final List<Uint8List> _pending = <Uint8List>[];

  Future<Uint8List> read(int count) async {
    final BytesBuilder builder = BytesBuilder(copy: false);
    while (builder.length < count) {
      if (_pending.isEmpty) {
        if (!await _iterator.moveNext()) {
          throw const SocketException('closed');
        }
        _pending.add(_iterator.current);
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

  Future<void> pumpTo(Socket sink) async {
    for (final Uint8List chunk in _pending) {
      sink.add(chunk);
    }
    _pending.clear();
    await sink.flush();
    while (await _iterator.moveNext()) {
      sink.add(_iterator.current);
      await sink.flush();
    }
  }

  Future<void> close() async {
    try {
      await _iterator.cancel();
    } catch (_) {
      // The peer may already have torn the socket down.
    }
  }
}
