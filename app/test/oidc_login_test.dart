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

  void respond(HttpRequest request, int status, Object? body) {
    request.response.statusCode = status;
    request.response.headers.contentType = ContentType.json;
    request.response.headers.set('cache-control', 'no-store');
    request.response.write(jsonEncode(body));
  }

  Map<String, Object?> loginBody() => <String, Object?>{
    'token': 'access-token',
    'expires_in_ms': 900000,
    'refresh_token': 'refresh-token',
    'refresh_expires_in_ms': 2592000000,
    'user': <String, Object?>{'username': 'alice'},
  };

  test('oidcInfo reports the configured provider', () async {
    final List<String> paths = <String>[];
    server.listen((HttpRequest request) async {
      paths.add(request.uri.path);
      respond(request, HttpStatus.ok, <String, Object?>{
        'enabled': true,
        'name': 'Example SSO',
        'auto_create': true,
      });
      await request.response.close();
    });

    final OidcLoginInfo info = await ServerApi.oidcInfo(settings);
    expect(paths, <String>['/v1/account/oidc']);
    expect(info.enabled, isTrue);
    expect(info.name, 'Example SSO');
    expect(info.autoCreate, isTrue);
  });

  test('oidcInfo treats an old server without the route as disabled', () async {
    server.listen((HttpRequest request) async {
      respond(request, HttpStatus.notFound, <String, Object?>{
        'error': 'not found',
      });
      await request.response.close();
    });

    final OidcLoginInfo info = await ServerApi.oidcInfo(settings);
    expect(info.enabled, isFalse);
  });

  test(
    'oidcBegin sends the device name and validates the provider URL',
    () async {
      final List<Object?> bodies = <Object?>[];
      server.listen((HttpRequest request) async {
        bodies.add(jsonDecode(await utf8.decoder.bind(request).join()));
        respond(request, HttpStatus.ok, <String, Object?>{
          'auth_url': 'https://idp.example.com/authorize?state=abc',
          'state': 'abc',
          'expires_in_ms': 600000,
        });
        await request.response.close();
      });

      final OidcLoginSession session = await ServerApi.oidcBegin(
        settings,
        deviceName: 'workstation',
      );
      expect(session.authUrl, 'https://idp.example.com/authorize?state=abc');
      expect(session.state, 'abc');
      expect(session.expiresInMs, 600000);
      expect(bodies, <Object?>[
        <String, Object?>{'device_id': '', 'device_name': 'workstation'},
      ]);
    },
  );

  test('oidcBegin rejects a non-http auth URL', () async {
    server.listen((HttpRequest request) async {
      respond(request, HttpStatus.ok, <String, Object?>{
        'auth_url': 'file:///etc/passwd',
        'state': 'abc',
        'expires_in_ms': 600000,
      });
      await request.response.close();
    });

    await expectLater(
      ServerApi.oidcBegin(settings),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException error) => error.message,
          'message',
          contains('无效'),
        ),
      ),
    );
  });

  test(
    'oidcPoll treats 202 as pending and 200 as the issued session',
    () async {
      final List<String> paths = <String>[];
      int calls = 0;
      server.listen((HttpRequest request) async {
        paths.add(request.uri.path);
        calls += 1;
        if (calls == 1) {
          respond(request, HttpStatus.accepted, <String, Object?>{
            'status': 'pending',
          });
        } else {
          respond(request, HttpStatus.ok, loginBody());
        }
        await request.response.close();
      });

      expect(await ServerApi.oidcPoll(settings, 'abc'), isNull);
      final AccountLoginResult? result = await ServerApi.oidcPoll(
        settings,
        'abc',
      );
      expect(result, isNotNull);
      expect(result!.token, 'access-token');
      expect(result.username, 'alice');
      expect(result.refreshToken, 'refresh-token');
      expect(paths, <String>['/v1/account/oidc/poll', '/v1/account/oidc/poll']);
    },
  );

  test('oidcPoll surfaces a terminal account error code', () async {
    server.listen((HttpRequest request) async {
      respond(request, HttpStatus.unauthorized, <String, Object?>{
        'error': 'no account is linked to this identity',
        'code': 'no_account',
      });
      await request.response.close();
    });

    await expectLater(
      ServerApi.oidcPoll(settings, 'abc'),
      throwsA(
        isA<ServerApiException>()
            .having(
              (ServerApiException error) => error.code,
              'code',
              'no_account',
            )
            .having(
              (ServerApiException error) => error.message,
              'message',
              contains('未关联账号'),
            ),
      ),
    );
  });

  test('loginOidc opens the provider and completes through polling', () async {
    final List<String> requested = <String>[];
    final List<String> opened = <String>[];
    int polls = 0;
    server.listen((HttpRequest request) async {
      requested.add(request.uri.path);
      switch (request.uri.path) {
        case '/v1/account/oidc/begin':
          respond(request, HttpStatus.ok, <String, Object?>{
            'auth_url': 'https://idp.example.com/authorize?state=abc',
            'state': 'abc',
            'expires_in_ms': 600000,
          });
        case '/v1/account/oidc/poll':
          polls += 1;
          if (polls < 2) {
            respond(request, HttpStatus.accepted, <String, Object?>{
              'status': 'pending',
            });
          } else {
            respond(request, HttpStatus.ok, loginBody());
          }
        default:
          respond(request, HttpStatus.notFound, <String, Object?>{
            'error': 'not found',
          });
      }
      await request.response.close();
    });

    final AccountLoginResult result = await ServerApi.loginOidc(
      settings,
      deviceName: 'workstation',
      openUrl: (String url) async => opened.add(url),
      interval: const Duration(milliseconds: 5),
      timeout: const Duration(seconds: 5),
    );

    expect(opened, <String>['https://idp.example.com/authorize?state=abc']);
    expect(result.token, 'access-token');
    expect(polls, 2);
    expect(requested.first, '/v1/account/oidc/begin');
  });

  test('loginOidc reports an unopened browser without polling', () async {
    final List<String> requested = <String>[];
    server.listen((HttpRequest request) async {
      requested.add(request.uri.path);
      respond(request, HttpStatus.ok, <String, Object?>{
        'auth_url': 'https://idp.example.com/authorize?state=abc',
        'state': 'abc',
        'expires_in_ms': 600000,
      });
      await request.response.close();
    });

    await expectLater(
      ServerApi.loginOidc(
        settings,
        openUrl: (String url) async => throw StateError('no browser available'),
        interval: const Duration(milliseconds: 5),
        timeout: const Duration(seconds: 1),
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException error) => error.message,
          'message',
          contains('无法打开浏览器'),
        ),
      ),
    );
    expect(requested, <String>['/v1/account/oidc/begin']);
  });

  test('loginOidc times out while the browser step stays pending', () async {
    server.listen((HttpRequest request) async {
      if (request.uri.path == '/v1/account/oidc/begin') {
        respond(request, HttpStatus.ok, <String, Object?>{
          'auth_url': 'https://idp.example.com/authorize?state=abc',
          'state': 'abc',
          'expires_in_ms': 600000,
        });
      } else {
        respond(request, HttpStatus.accepted, <String, Object?>{
          'status': 'pending',
        });
      }
      await request.response.close();
    });

    await expectLater(
      ServerApi.loginOidc(
        settings,
        openUrl: (String url) async {},
        interval: const Duration(milliseconds: 5),
        timeout: const Duration(milliseconds: 40),
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException error) => error.message,
          'message',
          contains('超时'),
        ),
      ),
    );
  });
}
