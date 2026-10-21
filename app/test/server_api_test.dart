import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  late HttpServer server;
  late ServerSettings settings;

  setUp(() async {
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    settings = ServerSettings(url: 'http://127.0.0.1:${server.port}');
  });

  tearDown(() async {
    await server.close(force: true);
  });

  test('lookup parses a host and keeps the requested id', () async {
    Uri? requested;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      request.response.headers.contentType = ContentType.json;
      request.response.write(jsonEncode(<String, Object?>{
        'online': true,
        'candidate': '203.0.113.7:13434',
        'relay': '198.51.100.2:5000',
        'fingerprint': 'AB' * 32,
        'last_seen_ms': 1234,
        'expires_in_ms': 9000,
      }));
      await request.response.close();
    });

    final HostInfo host = await ServerApi.lookup(settings, 'devbox');
    expect(requested?.path, '/v1/hosts/devbox');
    expect(host.id, 'devbox');
    expect(host.online, isTrue);
    expect(host.hasDirectCandidate, isTrue);
    expect(host.hasRelay, isTrue);
    expect(host.candidate, '203.0.113.7:13434');
    expect(host.fingerprint, 'AB' * 32);
    expect(host.lastSeenMs, 1234);
    expect(host.expiresInMs, 9000);
  });

  test('an unregistered host reports 404 as a typed error', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.notFound;
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookup(settings, 'ghost'),
      throwsA(isA<ServerApiException>()
          .having((ServerApiException e) => e.statusCode, 'statusCode', 404)
          .having((ServerApiException e) => e.message, 'message', contains('未注册'))),
    );
  });

  test('other HTTP failures carry the status', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.internalServerError;
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookup(settings, 'devbox'),
      throwsA(isA<ServerApiException>()
          .having((ServerApiException e) => e.statusCode, 'statusCode', 500)),
    );
  });

  test('invalid JSON is rejected', () async {
    server.listen((HttpRequest request) async {
      request.response.write('<html>not json</html>');
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookup(settings, 'devbox'),
      throwsA(isA<ServerApiException>()
          .having((ServerApiException e) => e.message, 'message', contains('JSON'))),
    );
  });

  test('health accepts the service status document', () async {
    server.listen((HttpRequest request) async {
      request.response.write('{"ok":true}');
      await request.response.close();
    });

    await ServerApi.health(settings);
  });

  test('unconfigured settings fail before opening a socket', () async {
    await expectLater(
      ServerApi.lookup(const ServerSettings(), 'devbox'),
      throwsA(isA<ServerApiException>()),
    );
  });
}
