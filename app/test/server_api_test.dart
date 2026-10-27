import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_identity.dart';
import 'package:chast_viewer/server_settings.dart';

const String _serverKey = 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8';

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
    String? authorization;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'online': true,
          'candidate': '203.0.113.7:13434',
          'relay': '198.51.100.2:5000',
          'fingerprint': 'AB' * 32,
          'server_public_key': _serverKey,
          'last_seen_ms': 1234,
          'expires_in_ms': 9000,
        }),
      );
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
    expect(host.serverPublicKey, _serverKey);
    expect(host.lastSeenMs, 1234);
    expect(host.expiresInMs, 9000);
    expect(authorization, isNull);
  });

  test('serverIdentity reads and fingerprints /v1/server', () async {
    Uri? requested;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{'public_key': _serverKey}),
      );
      await request.response.close();
    });

    final ServerIdentity identity = await ServerApi.serverIdentity(settings);
    expect(requested?.path, '/v1/server');
    expect(identity.publicKey, _serverKey);
    expect(
      identity.fingerprint,
      '630DCD2966C4336691125448BBB25B4FF412A49C732DB2C8ABC1B8581BD710DD',
    );
  });

  test('serverIdentity refuses a server without a usable key', () async {
    server.listen((HttpRequest request) async {
      request.response.headers.contentType = ContentType.json;
      request.response.write(jsonEncode(<String, Object?>{'version': '1'}));
      await request.response.close();
    });
    await expectLater(
      ServerApi.serverIdentity(settings),
      throwsA(isA<ServerApiException>()),
    );
  });

  test(
    'lookupBatch refreshes peers with alias, tags and missing ids',
    () async {
      Uri? requested;
      String? authorization;
      server.listen((HttpRequest request) async {
        requested = request.uri;
        authorization = request.headers.value(HttpHeaders.authorizationHeader);
        request.response.headers.contentType = ContentType.json;
        request.response.write(
          jsonEncode(<String, Object?>{
            'hosts': <Object?>[
              <String, Object?>{
                'id': 'studio',
                'online': true,
                'alias': '工作室',
                'tags': <String>['office', 'prod'],
              },
              <String, Object?>{'id': 'lab', 'online': false},
            ],
            'missing': <String>['gone'],
            'software': <String, Object?>{
              'version': '2.1.0',
              'url': 'https://downloads.example.com/chast-2.1.0.exe',
            },
          }),
        );
        await request.response.close();
      });

      final BatchLookupResult batch = await ServerApi.lookupBatch(
        settings.copyWith(key: 'dev-secret'),
        <String>['studio', 'lab', 'gone', 'studio', ' '],
      );
      final Map<String, HostInfo> hosts = batch.hosts;
      expect(requested?.path, '/v1/hosts');
      expect(requested?.queryParameters['ids'], 'studio,lab,gone');
      expect(authorization, 'Bearer dev-secret');
      expect(hosts.keys, unorderedEquals(<String>['studio', 'lab']));
      expect(hosts['studio']?.alias, '工作室');
      expect(hosts['studio']?.tags, <String>['office', 'prod']);
      expect(hosts['lab']?.online, isFalse);
      expect(batch.software?.version, '2.1.0');
      expect(
        batch.software?.url,
        'https://downloads.example.com/chast-2.1.0.exe',
      );
    },
  );

  test('lookupBatch splits requests at the server limit', () async {
    final List<String> batches = <String>[];
    server.listen((HttpRequest request) async {
      batches.add(request.uri.queryParameters['ids'] ?? '');
      request.response.headers.contentType = ContentType.json;
      request.response.write('{"hosts":[]}');
      await request.response.close();
    });

    final List<String> ids = <String>[
      for (int i = 0; i < ServerApi.maxBatchIds + 1; ++i) 'host$i',
    ];
    final BatchLookupResult batch = await ServerApi.lookupBatch(settings, ids);
    expect(batch.hosts, isEmpty);
    expect(batches, hasLength(2));
    expect(batches.first.split(','), hasLength(ServerApi.maxBatchIds));
    expect(batches.last.split(','), hasLength(1));
  });

  test('lookupBatch ignores an incomplete software update offer', () async {
    server.listen((HttpRequest request) async {
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        '{"hosts":[],"software":{"version":"2.1.0","url":""}}',
      );
      await request.response.close();
    });

    final BatchLookupResult batch = await ServerApi.lookupBatch(
      settings,
      <String>['devbox'],
    );
    expect(batch.software, isNull);
  });

  test('lookupBatch rejects a response without a host list', () async {
    server.listen((HttpRequest request) async {
      request.response.write('{"missing":["devbox"]}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookupBatch(settings, <String>['devbox']),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('无效'),
        ),
      ),
    );
  });

  test('lookup sends the server key as a bearer token', () async {
    String? authorization;
    server.listen((HttpRequest request) async {
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      request.response.headers.contentType = ContentType.json;
      request.response.write('{"online":false}');
      await request.response.close();
    });

    await ServerApi.lookup(settings.copyWith(key: 'dev-secret'), 'devbox');
    expect(authorization, 'Bearer dev-secret');
  });

  test('a rejected server key is a typed 401', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.unauthorized;
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookup(settings.copyWith(key: 'wrong'), 'devbox'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 401)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('密钥'),
            ),
      ),
    );
  });

  test('updateLabels patches alias and tags with bearer auth', () async {
    String? method;
    Uri? requested;
    String? authorization;
    String? contentType;
    Map<String, Object?>? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      contentType = request.headers.contentType?.mimeType;
      body =
          jsonDecode(await utf8.decoder.bind(request).join())
              as Map<String, Object?>;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'id': 'devbox',
          'alias': '设计室',
          'tags': <String>['office', 'design'],
        }),
      );
      await request.response.close();
    });

    await ServerApi.updateLabels(
      settings.copyWith(key: 'dev-secret'),
      ' devbox ',
      alias: ' 设计室 ',
      tags: <String>['office', ' office ', '', 'design'],
    );
    expect(method, 'PATCH');
    expect(requested?.path, '/v1/identities/devbox');
    expect(authorization, 'Bearer dev-secret');
    expect(contentType, 'application/json');
    expect(body?['alias'], '设计室');
    expect(body?['tags'], <String>['office', 'design']);
  });

  test('updateLabels surfaces a rejected server key', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.unauthorized;
      await request.response.close();
    });

    await expectLater(
      ServerApi.updateLabels(
        settings.copyWith(key: 'wrong'),
        'devbox',
        alias: '',
        tags: const <String>[],
      ),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 401)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('密钥'),
            ),
      ),
    );
  });

  test('updateLabels rejects a response for another identity', () async {
    server.listen((HttpRequest request) async {
      request.response.headers.contentType = ContentType.json;
      request.response.write('{"id":"other"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.updateLabels(
        settings,
        'devbox',
        alias: 'x',
        tags: const <String>[],
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('无效'),
        ),
      ),
    );
  });

  test('check treats an authorized 404 as a healthy service', () async {
    Uri? requested;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      request.response.statusCode = HttpStatus.notFound;
      await request.response.close();
    });

    await ServerApi.check(settings.copyWith(key: 'dev-secret'));
    expect(requested?.path, '/v1/hosts/chast-probe');
  });

  test('check surfaces a rejected key', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.unauthorized;
      await request.response.close();
    });

    await expectLater(
      ServerApi.check(settings.copyWith(key: 'wrong')),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.statusCode,
          'statusCode',
          401,
        ),
      ),
    );
  });

  test('an unregistered host reports 404 as a typed error', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.notFound;
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookup(settings, 'ghost'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 404)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('未注册'),
            ),
      ),
    );
  });

  test('other HTTP failures carry the status', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.internalServerError;
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookup(settings, 'devbox'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.statusCode,
          'statusCode',
          500,
        ),
      ),
    );
  });

  test('invalid JSON is rejected', () async {
    server.listen((HttpRequest request) async {
      request.response.write('<html>not json</html>');
      await request.response.close();
    });

    await expectLater(
      ServerApi.lookup(settings, 'devbox'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('JSON'),
        ),
      ),
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

  test('login posts credentials and parses the account token', () async {
    String? method;
    Uri? requested;
    String? authorization;
    Object? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'token': 'account-token',
          'expires_in_ms': 1800000,
          'refresh_token': 'refresh-token',
          'refresh_expires_in_ms': 2592000000,
          'user': <String, Object?>{'id': 'user-1', 'username': 'alice'},
        }),
      );
      await request.response.close();
    });

    final AccountLoginResult result = await ServerApi.login(
      settings.copyWith(key: 'operator-key'),
      username: ' alice ',
      password: 'secret123',
    );
    expect(method, 'POST');
    expect(requested?.path, '/v1/account/login');
    expect(authorization, isNull);
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
    });
    expect(result.token, 'account-token');
    expect(result.username, 'alice');
    expect(result.expiresInMs, 1800000);
    expect(result.refreshToken, 'refresh-token');
    expect(result.refreshExpiresInMs, 2592000000);
  });

  test('refresh rotates the token pair without credentials', () async {
    Uri? requested;
    String? authorization;
    Object? body;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'token': 'new-access',
          'expires_in_ms': 1800000,
          'refresh_token': 'new-refresh',
          'refresh_expires_in_ms': 2592000000,
          'user': <String, Object?>{'id': 'user-1', 'username': 'alice'},
        }),
      );
      await request.response.close();
    });

    final AccountLoginResult result =
        await ServerApi.refresh(settings, ' old-refresh ');
    expect(requested?.path, '/v1/account/refresh');
    expect(authorization, isNull);
    expect(body, <String, Object?>{'refresh_token': 'old-refresh'});
    expect(result.token, 'new-access');
    expect(result.refreshToken, 'new-refresh');
    expect(result.username, 'alice');
  });

  test('refresh maps a rejected token to a signed-out error', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.unauthorized;
      request.response.write('{"error":"refresh token was not accepted"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.refresh(settings, 'retired-refresh'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 401)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('重新登录'),
            ),
      ),
    );
  });

  test('login reports bad credentials as an account error', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.unauthorized;
      request.response.write(
        '{"error":"username or password was not accepted"}',
      );
      await request.response.close();
    });

    await expectLater(
      ServerApi.login(settings, username: 'alice', password: 'wrong'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 401)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('用户名或密码'),
            ),
      ),
    );
  });

  test('login sends the device name and surfaces lockouts', () async {
    Object? body;
    server.listen((HttpRequest request) async {
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.statusCode = HttpStatus.tooManyRequests;
      request.response.write('{"error":"too many failed logins"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.login(
        settings,
        username: 'alice',
        password: 'secret123',
        deviceName: ' 书房-PC ',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.statusCode,
          'statusCode',
          429,
        ),
      ),
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'device_name': '书房-PC',
    });
  });

  test('accountSessions parses devices and marks the current one', () async {
    Uri? requested;
    String? authorization;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'sessions': <Object?>[
            <String, Object?>{
              'id': 's1',
              'device_id': 'dev-1',
              'device_name': '书房-PC',
              'created_at_ms': 1761300000000,
              'expires_at_ms': 1764000000000,
              'current': true,
            },
            <String, Object?>{
              'id': 's2',
              'device_name': '笔记本',
              'created_at_ms': 1761400000000,
              'expires_at_ms': 1764100000000,
            },
          ],
        }),
      );
      await request.response.close();
    });

    final List<AccountDeviceSession> sessions = await ServerApi.accountSessions(
      settings,
      'account-token',
    );
    expect(requested?.path, '/v1/account/sessions');
    expect(authorization, 'Bearer account-token');
    expect(sessions, hasLength(2));
    expect(sessions[0].id, 's1');
    expect(sessions[0].deviceId, 'dev-1');
    expect(sessions[0].deviceName, '书房-PC');
    expect(sessions[0].current, isTrue);
    expect(
      sessions[0].createdAt,
      DateTime.fromMillisecondsSinceEpoch(1761300000000, isUtc: true),
    );
    expect(sessions[1].current, isFalse);
    expect(sessions[1].deviceName, '笔记本');
  });

  test('revokeAccountSession deletes one session by id', () async {
    String? method;
    Uri? requested;
    String? authorization;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      request.response.statusCode = HttpStatus.noContent;
      await request.response.close();
    });

    await ServerApi.revokeAccountSession(settings, 'account-token', 's2');
    expect(method, 'DELETE');
    expect(requested?.path, '/v1/account/sessions/s2');
    expect(authorization, 'Bearer account-token');
  });

  test('renameAccountSession patches the trimmed device name', () async {
    String? method;
    Uri? requested;
    String? authorization;
    Object? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.statusCode = HttpStatus.noContent;
      await request.response.close();
    });

    await ServerApi.renameAccountSession(
      settings,
      'account-token',
      's2',
      deviceName: '  书房 PC  ',
    );
    expect(method, 'PATCH');
    expect(requested?.path, '/v1/account/sessions/s2');
    expect(authorization, 'Bearer account-token');
    expect(body, <String, Object?>{'device_name': '书房 PC'});
  });

  test('renameAccountSession rejects an empty name before sending', () async {
    await expectLater(
      ServerApi.renameAccountSession(
        settings,
        'account-token',
        's2',
        deviceName: '   ',
      ),
      throwsA(isA<ServerApiException>()),
    );
  });

  test(
    'revokeOtherAccountSessions posts and returns the removed count',
    () async {
      String? method;
      Uri? requested;
      String? authorization;
      server.listen((HttpRequest request) async {
        method = request.method;
        requested = request.uri;
        authorization = request.headers.value(HttpHeaders.authorizationHeader);
        request.response.headers.contentType = ContentType.json;
        request.response.write(jsonEncode(<String, Object?>{'revoked': 2}));
        await request.response.close();
      });

      final int revoked = await ServerApi.revokeOtherAccountSessions(
        settings,
        'account-token',
      );
      expect(revoked, 2);
      expect(method, 'POST');
      expect(requested?.path, '/v1/account/sessions/revoke-others');
      expect(authorization, 'Bearer account-token');
    },
  );

  test('changePassword posts both secrets and accepts 204', () async {
    String? method;
    Uri? requested;
    String? authorization;
    Object? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.statusCode = HttpStatus.noContent;
      await request.response.close();
    });

    await ServerApi.changePassword(
      settings,
      'account-token',
      currentPassword: 'old secret',
      newPassword: 'new secret 1',
    );
    expect(method, 'POST');
    expect(requested?.path, '/v1/account/password');
    expect(authorization, 'Bearer account-token');
    expect(body, <String, Object?>{
      'current_password': 'old secret',
      'new_password': 'new secret 1',
    });
  });

  test('changePassword reports a rejected current password', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.forbidden;
      request.response.write('{"error":"current password was not accepted"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.changePassword(
        settings,
        'account-token',
        currentPassword: 'wrong',
        newPassword: 'new secret 1',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '当前密码不正确',
        ),
      ),
    );
  });

  test('login sends the two-factor code when one is supplied', () async {
    Object? body;
    server.listen((HttpRequest request) async {
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'token': 'account-token',
          'expires_in_ms': 1000,
          'user': <String, Object?>{'username': 'alice'},
        }),
      );
      await request.response.close();
    });

    await ServerApi.login(
      settings,
      username: 'alice',
      password: 'secret123',
      totpCode: ' 123456 ',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'totp_code': '123456',
    });
  });

  test('login maps the two-factor header to actionable errors', () async {
    const List<String> headers = <String>['required', 'invalid'];
    int calls = 0;
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.unauthorized;
      request.response.headers.set('x-chast-2fa', headers[calls++]);
      request.response.write('{"error":"two-factor"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.login(settings, username: 'alice', password: 'secret123'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.code, 'code', 'totp_required')
            .having((ServerApiException e) => e.message, 'message', '请输入两步验证码'),
      ),
    );
    await expectLater(
      ServerApi.login(
        settings,
        username: 'alice',
        password: 'secret123',
        totpCode: '000000',
      ),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.code, 'code', 'totp_invalid')
            .having((ServerApiException e) => e.message, 'message', '验证码或恢复码不正确'),
      ),
    );
    expect(calls, 2);
  });

  test('login sends a recovery code instead of the authenticator code', () async {
    Object? body;
    server.listen((HttpRequest request) async {
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'token': 'account-token',
          'expires_in_ms': 1000,
          'user': <String, Object?>{'username': 'alice'},
        }),
      );
      await request.response.close();
    });

    await ServerApi.login(
      settings,
      username: 'alice',
      password: 'secret123',
      totpCode: '123456',
      recoveryCode: ' abcd-efgh-ijkl-mnop ',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'totp_code': '123456',
    });

    await ServerApi.login(
      settings,
      username: 'alice',
      password: 'secret123',
      recoveryCode: ' abcd-efgh-ijkl-mnop ',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'recovery_code': 'abcd-efgh-ijkl-mnop',
    });
  });

  test('accountInfo reads the two-factor flag', () async {
    Uri? requested;
    String? authorization;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'user': <String, Object?>{'id': 'u1', 'username': 'alice'},
          'two_factor': true,
          'recovery_codes': 4,
        }),
      );
      await request.response.close();
    });

    final AccountInfo info = await ServerApi.accountInfo(
      settings,
      'account-token',
    );
    expect(requested?.path, '/v1/account');
    expect(authorization, 'Bearer account-token');
    expect(info.username, 'alice');
    expect(info.twoFactor, isTrue);
    expect(info.recoveryCodes, 4);
  });

  test('startTwoFactor posts an enrollment and parses the secret', () async {
    String? method;
    Uri? requested;
    String? authorization;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'secret': 'JBSWY3DPEHPK3PXP',
          'otpauth_uri': 'otpauth://totp/chast:alice?secret=JBSWY3DPEHPK3PXP',
        }),
      );
      await request.response.close();
    });

    final TwoFactorEnrollment enrollment = await ServerApi.startTwoFactor(
      settings,
      'account-token',
    );
    expect(method, 'POST');
    expect(requested?.path, '/v1/account/2fa/start');
    expect(authorization, 'Bearer account-token');
    expect(enrollment.secret, 'JBSWY3DPEHPK3PXP');
    expect(enrollment.uri, startsWith('otpauth://totp/'));
  });

  test(
    'startTwoFactor explains that enabled two-factor must be removed first',
    () async {
      server.listen((HttpRequest request) async {
        request.response.statusCode = HttpStatus.conflict;
        request.response.write('{"error":"two-factor is already enabled"}');
        await request.response.close();
      });

      await expectLater(
        ServerApi.startTwoFactor(settings, 'account-token'),
        throwsA(
          isA<ServerApiException>().having(
            (ServerApiException e) => e.message,
            'message',
            contains('已启用'),
          ),
        ),
      );
    },
  );

  test('enableTwoFactor posts the code and accepts 204', () async {
    String? method;
    Uri? requested;
    String? authorization;
    Object? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.statusCode = HttpStatus.noContent;
      await request.response.close();
    });

    await ServerApi.enableTwoFactor(settings, 'account-token', ' 123456 ');
    expect(method, 'POST');
    expect(requested?.path, '/v1/account/2fa/enable');
    expect(authorization, 'Bearer account-token');
    expect(body, <String, Object?>{'code': '123456'});
  });

  test(
    'enableTwoFactor maps a rejected code and a missing enrollment',
    () async {
      int calls = 0;
      server.listen((HttpRequest request) async {
        calls += 1;
        if (calls == 1) {
          request.response.statusCode = HttpStatus.forbidden;
          request.response.write('{"error":"code"}');
        } else {
          request.response.statusCode = HttpStatus.badRequest;
          request.response.write('{"error":"not started"}');
        }
        await request.response.close();
      });

      await expectLater(
        ServerApi.enableTwoFactor(settings, 'account-token', '000000'),
        throwsA(
          isA<ServerApiException>().having(
            (ServerApiException e) => e.message,
            'message',
            '两步验证码不正确',
          ),
        ),
      );
      await expectLater(
        ServerApi.enableTwoFactor(settings, 'account-token', '000000'),
        throwsA(
          isA<ServerApiException>().having(
            (ServerApiException e) => e.message,
            'message',
            '请先获取两步验证密钥',
          ),
        ),
      );
    },
  );

  test(
    'disableTwoFactor posts the password and code and accepts 204',
    () async {
      String? method;
      Uri? requested;
      String? authorization;
      Object? body;
      server.listen((HttpRequest request) async {
        method = request.method;
        requested = request.uri;
        authorization = request.headers.value(HttpHeaders.authorizationHeader);
        body = jsonDecode(await utf8.decoder.bind(request).join());
        request.response.statusCode = HttpStatus.noContent;
        await request.response.close();
      });

      await ServerApi.disableTwoFactor(
        settings,
        'account-token',
        password: 'secret123',
        code: ' 654321 ',
      );
      expect(method, 'POST');
      expect(requested?.path, '/v1/account/2fa/disable');
      expect(authorization, 'Bearer account-token');
      expect(body, <String, Object?>{
        'password': 'secret123',
        'code': '654321',
      });
    },
  );

  test(
    'disableTwoFactor reports rejected credentials and disabled state',
    () async {
      int calls = 0;
      server.listen((HttpRequest request) async {
        calls += 1;
        if (calls == 1) {
          request.response.statusCode = HttpStatus.forbidden;
          request.response.write('{"error":"credentials"}');
        } else {
          request.response.statusCode = HttpStatus.badRequest;
          request.response.write('{"error":"not enabled"}');
        }
        await request.response.close();
      });

      await expectLater(
        ServerApi.disableTwoFactor(
          settings,
          'account-token',
          password: 'wrong',
          code: '000000',
        ),
        throwsA(
          isA<ServerApiException>().having(
            (ServerApiException e) => e.message,
            'message',
            '密码或两步验证码不正确',
          ),
        ),
      );
      await expectLater(
        ServerApi.disableTwoFactor(
          settings,
          'account-token',
          password: 'secret123',
          code: '000000',
        ),
        throwsA(
          isA<ServerApiException>().having(
            (ServerApiException e) => e.message,
            'message',
            '两步验证未启用',
          ),
        ),
      );
    },
  );

  test('generateRecoveryCodes posts the proof and returns the codes', () async {
    String? method;
    Uri? requested;
    String? authorization;
    Object? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'codes': <String>['ABCD-EFGH-IJKL-MNOP', 'QRST-UVWX-YZ23-4567'],
        }),
      );
      await request.response.close();
    });

    final List<String> codes = await ServerApi.generateRecoveryCodes(
      settings,
      'account-token',
      password: 'secret123',
      code: ' 654321 ',
    );
    expect(method, 'POST');
    expect(requested?.path, '/v1/account/2fa/recovery');
    expect(authorization, 'Bearer account-token');
    expect(body, <String, Object?>{
      'password': 'secret123',
      'code': '654321',
    });
    expect(codes, <String>['ABCD-EFGH-IJKL-MNOP', 'QRST-UVWX-YZ23-4567']);
  });

  test('generateRecoveryCodes maps rejected proofs and disabled state', () async {
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls += 1;
      if (calls == 1) {
        request.response.statusCode = HttpStatus.forbidden;
        request.response.write('{"error":"credentials"}');
      } else {
        request.response.statusCode = HttpStatus.badRequest;
        request.response.write('{"error":"not enabled"}');
      }
      await request.response.close();
    });

    await expectLater(
      ServerApi.generateRecoveryCodes(
        settings,
        'account-token',
        password: 'wrong',
        code: '000000',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '密码或两步验证码不正确',
        ),
      ),
    );
    await expectLater(
      ServerApi.generateRecoveryCodes(
        settings,
        'account-token',
        password: 'secret123',
        code: '000000',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '两步验证未启用',
        ),
      ),
    );
  });

  test('generateRecoveryCodes rejects an empty code before sending', () async {
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls += 1;
      await request.response.close();
    });

    await expectLater(
      ServerApi.generateRecoveryCodes(
        settings,
        'account-token',
        password: 'secret123',
        code: '  ',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '请输入两步验证码',
        ),
      ),
    );
    expect(calls, 0);
  });

  test('address book uses the account token and parses peers', () async {
    String? authorization;
    Uri? requested;
    server.listen((HttpRequest request) async {
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      requested = request.uri;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'revision': 7,
          'peers': <Object?>[
            <String, Object?>{
              'id': 'devbox',
              'name': '办公室',
              'tags': <String>['lab', 'vip'],
              'group_id': 'work',
              'updated_at_ms': 1761300000000,
              'revision': 6,
            },
            <String, Object?>{'id': 'lab'},
          ],
          'groups': <Object?>[
            <String, Object?>{
              'id': 'work',
              'name': 'Work',
              'updated_at_ms': 1761300000000,
              'revision': 4,
            },
            <String, Object?>{'id': 'lab', 'name': 'Lab'},
          ],
          'group_changes': <Object?>[
            <String, Object?>{'id': 'oldgroup', 'revision': 7, 'deleted': true},
          ],
          'changes': <Object?>[
            <String, Object?>{
              'id': 'oldbox',
              'revision': 7,
              'deleted': true,
              'updated_at_ms': 1761300001000,
            },
          ],
        }),
      );
      await request.response.close();
    });

    final AddressBookSnapshot snapshot = await ServerApi.addressBook(
      settings.copyWith(key: 'operator-key'),
      'account-token',
      since: 5,
    );
    expect(authorization, 'Bearer account-token');
    expect(requested?.queryParameters['since'], '5');
    expect(snapshot.revision, 7);
    expect(snapshot.peers, hasLength(2));
    expect(snapshot.peers.first.id, 'devbox');
    expect(snapshot.peers.first.name, '办公室');
    expect(snapshot.peers.first.tags, <String>['lab', 'vip']);
    expect(snapshot.peers.first.updatedAtMs, 1761300000000);
    expect(snapshot.peers.first.revision, 6);
    expect(snapshot.peers.first.groupId, 'work');
    expect(snapshot.peers.last.name, '');
    expect(snapshot.peers.last.groupId, '');
    expect(snapshot.changes, hasLength(1));
    expect(snapshot.tombstones.single.id, 'oldbox');
    expect(snapshot.tombstones.single.deleted, isTrue);
    expect(snapshot.groups, hasLength(2));
    expect(snapshot.groups.first.id, 'work');
    expect(snapshot.groups.first.name, 'Work');
    expect(snapshot.groups.first.revision, 4);
    expect(snapshot.groupChanges.single.id, 'oldgroup');
    expect(snapshot.groupTombstones.single.deleted, isTrue);
  });

  test('address book accepts an older server without revisions', () async {
    server.listen((HttpRequest request) async {
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'peers': <Object?>[
            <String, Object?>{'id': 'devbox', 'name': '办公室'},
          ],
        }),
      );
      await request.response.close();
    });

    final AddressBookSnapshot snapshot = await ServerApi.addressBook(
      settings,
      'account-token',
    );
    expect(snapshot.revision, 0);
    expect(snapshot.peers.single.id, 'devbox');
    expect(snapshot.changes, isEmpty);
  });

  test('address book update sends partial fields and checks the id', () async {
    String? method;
    Object? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'id': 'devbox',
          'name': '办公室',
          'tags': <String>['lab', 'vip'],
        }),
      );
      await request.response.close();
    });

    final AddressBookPeer peer = await ServerApi.updateAddressBookPeer(
      settings,
      'account-token',
      'devbox',
      tags: <String>['lab', ' lab ', 'vip'],
      ifRevision: 4,
    );
    expect(method, 'PATCH');
    expect(body, <String, Object?>{
      'tags': <String>['lab', 'vip'],
      'if_revision': 4,
    });
    expect(peer.name, '办公室');

    await expectLater(
      ServerApi.updateAddressBookPeer(settings, 'account-token', 'devbox'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('字段'),
        ),
      ),
    );
  });

  test('address book delete accepts 204 and reports a missing entry', () async {
    int calls = 0;
    String? method;
    server.listen((HttpRequest request) async {
      calls++;
      method = request.method;
      if (calls == 1) {
        request.response.statusCode = HttpStatus.noContent;
      } else {
        request.response.statusCode = HttpStatus.notFound;
      }
      await request.response.close();
    });

    await ServerApi.deleteAddressBookPeer(settings, 'account-token', 'devbox');
    expect(method, 'DELETE');
    await expectLater(
      ServerApi.deleteAddressBookPeer(settings, 'account-token', 'devbox'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 404)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('不存在'),
            ),
      ),
    );
  });

  test('address book groups are created, deleted and validated', () async {
    final List<String> requests = <String>[];
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls++;
      final String body = await utf8.decoder.bind(request).join();
      requests.add('${request.method} ${request.uri.path} $body');
      request.response.headers.contentType = ContentType.json;
      if (request.method == 'PATCH') {
        request.response.write(
          jsonEncode(<String, Object?>{
            'id': 'work',
            'name': 'Work',
            'updated_at_ms': 1761300000000,
            'revision': 6,
          }),
        );
      } else if (calls == 2) {
        request.response.statusCode = HttpStatus.noContent;
      } else {
        request.response.statusCode = HttpStatus.notFound;
        request.response.write('{"error":"address book group is unknown"}');
      }
      await request.response.close();
    });

    final AddressBookGroup group = await ServerApi.updateAddressBookGroup(
      settings,
      'account-token',
      ' work ',
      ' Work ',
    );
    expect(
      requests.single,
      'PATCH /v1/addressbook/groups/work {"name":"Work"}',
    );
    expect(group.id, 'work');
    expect(group.name, 'Work');
    expect(group.revision, 6);

    await ServerApi.deleteAddressBookGroup(settings, 'account-token', 'work');
    expect(requests.last, 'DELETE /v1/addressbook/groups/work ');
    await expectLater(
      ServerApi.deleteAddressBookGroup(settings, 'account-token', 'work'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 404)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('分组不存在'),
            ),
      ),
    );
    await expectLater(
      ServerApi.updateAddressBookGroup(
        settings,
        'account-token',
        'work',
        '   ',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('分组名称'),
        ),
      ),
    );
    await expectLater(
      ServerApi.deleteAddressBookGroup(settings, 'account-token', '  '),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('分组 ID'),
        ),
      ),
    );
  });

  test('address book peer group moves send only the group id', () async {
    Object? body;
    server.listen((HttpRequest request) async {
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{'id': 'devbox', 'group_id': 'work'}),
      );
      await request.response.close();
    });

    final AddressBookPeer peer = await ServerApi.updateAddressBookPeer(
      settings,
      'account-token',
      'devbox',
      groupId: ' work ',
    );
    expect(body, <String, Object?>{'group_id': 'work'});
    expect(peer.groupId, 'work');

    body = null;
    await ServerApi.updateAddressBookPeer(
      settings,
      'account-token',
      'devbox',
      groupId: '',
    );
    expect(body, <String, Object?>{'group_id': ''});
  });

  test('shared address books parse grants and peers', () async {
    String? authorization;
    final List<String> paths = <String>[];
    server.listen((HttpRequest request) async {
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      paths.add(request.uri.path);
      request.response.headers.contentType = ContentType.json;
      if (request.uri.path.endsWith('/grants')) {
        request.response.write(
          jsonEncode(<String, Object?>{
            'outgoing': <Object?>[
              <String, Object?>{
                'username': 'bob',
                'permission': 'write',
                'created_at_ms': 10,
              },
            ],
            'incoming': <Object?>[
              <String, Object?>{'username': 'carol', 'created_at_ms': 20},
            ],
          }),
        );
      } else {
        request.response.write(
          jsonEncode(<String, Object?>{
            'books': <Object?>[
              <String, Object?>{
                'owner': 'carol',
                'permission': 'control',
                'revision': 9,
                'peers': <Object?>[
                  <String, Object?>{
                    'id': 'sharedbox',
                    'name': '共享设备',
                    'tags': <String>['shared'],
                    'revision': 8,
                  },
                ],
              },
            ],
          }),
        );
      }
      await request.response.close();
    });

    final AddressBookGrants grants = await ServerApi.addressBookGrants(
      settings.copyWith(key: 'operator-key'),
      ' account-token ',
    );
    expect(authorization, 'Bearer account-token');
    expect(grants.outgoing.single.username, 'bob');
    expect(grants.outgoing.single.permission, 'write');
    expect(grants.outgoing.single.createdAtMs, 10);
    expect(grants.incoming.single.username, 'carol');
    expect(grants.incoming.single.permission, 'read');
    final List<SharedAddressBook> books = await ServerApi.sharedAddressBooks(
      settings,
      'account-token',
    );
    expect(books.single.owner, 'carol');
    expect(books.single.permission, 'control');
    expect(books.single.revision, 9);
    expect(books.single.peers.single.id, 'sharedbox');
    expect(books.single.peers.single.name, '共享设备');
    expect(books.single.peers.single.tags, <String>['shared']);
    expect(paths, <String>['/v1/addressbook/grants', '/v1/addressbook/shared']);
  });

  test('sharing posts a username and revoking accepts 204', () async {
    final List<String> methods = <String>[];
    final List<String> paths = <String>[];
    Object? body;
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls++;
      methods.add(request.method);
      paths.add(request.uri.path);
      if (request.method == 'POST') {
        body = jsonDecode(await utf8.decoder.bind(request).join());
        request.response.headers.contentType = ContentType.json;
        request.response.write(
          jsonEncode(<String, Object?>{'username': 'bob', 'created_at_ms': 30}),
        );
      } else {
        request.response.statusCode = HttpStatus.noContent;
      }
      await request.response.close();
    });

    final AddressBookGrant grant = await ServerApi.shareAddressBook(
      settings,
      'account-token',
      ' bob ',
    );
    expect(body, <String, Object?>{'username': 'bob', 'permission': 'read'});
    expect(grant.username, 'bob');
    expect(grant.permission, 'read');
    expect(grant.createdAtMs, 30);
    await ServerApi.unshareAddressBook(settings, 'account-token', 'BOB');
    expect(methods, <String>['POST', 'DELETE']);
    expect(paths.last, '/v1/addressbook/grants/BOB');
    expect(calls, 2);
    await expectLater(
      ServerApi.shareAddressBook(settings, 'account-token', '  '),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('用户名'),
        ),
      ),
    );
    await expectLater(
      ServerApi.shareAddressBook(
        settings,
        'account-token',
        'bob',
        permission: 'admin',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('权限无效'),
        ),
      ),
    );
    expect(calls, 2);
  });

  test('address book permissions rank read below write below control', () {
    expect(addressBookPermissionAllows('read', 'read'), isTrue);
    expect(addressBookPermissionAllows('read', 'write'), isFalse);
    expect(addressBookPermissionAllows('write', 'read'), isTrue);
    expect(addressBookPermissionAllows('write', 'control'), isFalse);
    expect(addressBookPermissionAllows('control', 'write'), isTrue);
    expect(addressBookPermissionAllows('control', 'control'), isTrue);
  });

  test('sharing a missing user reports 404', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.notFound;
      request.response.write('{"error":"user is unknown"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.shareAddressBook(settings, 'account-token', 'nobody'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 404)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('用户或分组不存在'),
            ),
      ),
    );
    await expectLater(
      ServerApi.unshareAddressBook(settings, 'account-token', 'nobody'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 404)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('没有共享'),
            ),
      ),
    );
  });

  test('shared book writes and grants use the shared paths', () async {
    final List<String> requests = <String>[];
    final List<String?> deleteRevisions = <String?>[];
    server.listen((HttpRequest request) async {
      final String body = await utf8.decoder.bind(request).join();
      requests.add('${request.method} ${request.uri.path} $body');
      if (request.method == 'DELETE') {
        deleteRevisions.add(request.uri.queryParameters['if_revision']);
      }
      request.response.headers.contentType = ContentType.json;
      switch (request.method) {
        case 'PATCH':
          request.response.write(
            jsonEncode(<String, Object?>{
              'id': 'sharedbox',
              'name': '会议室',
              'tags': <String>['lab', 'vip'],
            }),
          );
        case 'DELETE':
          request.response.statusCode = HttpStatus.noContent;
        case 'GET':
          request.response.write(
            jsonEncode(<String, Object?>{
              'outgoing': <Object?>[
                <String, Object?>{
                  'username': 'dave',
                  'permission': 'write',
                  'created_at_ms': 40,
                },
              ],
              'incoming': <Object?>[
                <String, Object?>{
                  'username': 'carol',
                  'permission': 'control',
                  'created_at_ms': 20,
                },
              ],
            }),
          );
        case 'POST':
          if (request.uri.path.endsWith('/peers')) {
            request.response.write(
              jsonEncode(<String, Object?>{
                'id': 'dlbox',
                'name': '下载机',
                'tags': <String>['lab', 'vip'],
                'updated_at_ms': 50,
                'revision': 51,
              }),
            );
          } else {
            request.response.write(
              jsonEncode(<String, Object?>{
                'username': 'dave',
                'permission': 'write',
                'created_at_ms': 40,
              }),
            );
          }
      }
      await request.response.close();
    });

    final AddressBookPeer peer = await ServerApi.updateSharedAddressBookPeer(
      settings,
      'account-token',
      ' carol ',
      'sharedbox',
      name: '会议室',
      tags: <String>['lab', ' lab ', 'vip'],
      ifRevision: 7,
    );
    expect(peer.name, '会议室');
    expect(peer.tags, <String>['lab', 'vip']);
    await ServerApi.deleteSharedAddressBookPeer(
      settings,
      'account-token',
      'carol',
      'sharedbox',
      ifRevision: 7,
    );
    final AddressBookGrants grants = await ServerApi.sharedAddressBookGrants(
      settings,
      'account-token',
      'carol',
    );
    expect(grants.outgoing.single.username, 'dave');
    expect(grants.outgoing.single.permission, 'write');
    expect(grants.incoming.single.permission, 'control');
    final AddressBookGrant grant = await ServerApi.shareSharedAddressBook(
      settings,
      'account-token',
      'carol',
      ' dave ',
      permission: 'write',
    );
    expect(grant.permission, 'write');
    final AddressBookPeer added = await ServerApi.addSharedAddressBookPeer(
      settings,
      'account-token',
      'carol',
      ' dlbox ',
      name: '下载机',
      tags: <String>['lab', ' lab ', 'vip'],
    );
    expect(added.id, 'dlbox');
    expect(added.name, '下载机');
    await ServerApi.unshareSharedAddressBook(
      settings,
      'account-token',
      'carol',
      'Dave',
    );

    expect(deleteRevisions, <String?>['7', null]);
    expect(requests, <String>[
      'PATCH /v1/addressbook/shared/carol/peers/sharedbox '
          '{"name":"会议室","tags":["lab","vip"],"if_revision":7}',
      'DELETE /v1/addressbook/shared/carol/peers/sharedbox ',
      'GET /v1/addressbook/shared/carol/grants ',
      'POST /v1/addressbook/shared/carol/grants '
          '{"username":"dave","permission":"write"}',
      'POST /v1/addressbook/shared/carol/peers '
          '{"id":"dlbox","name":"下载机","tags":["lab","vip"]}',
      'DELETE /v1/addressbook/shared/carol/grants/Dave ',
    ]);
    await expectLater(
      ServerApi.addSharedAddressBookPeer(
        settings,
        'account-token',
        'carol',
        ' ',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '设备 ID 为空',
        ),
      ),
    );
  });

  test('group scoped shares carry the scope on the wire', () async {
    final List<String> requests = <String>[];
    server.listen((HttpRequest request) async {
      final String body = await utf8.decoder.bind(request).join();
      requests.add(
        '${request.method} ${request.uri.path}'
        '${request.uri.hasQuery ? '?${request.uri.query}' : ''} $body',
      );
      request.response.headers.contentType = ContentType.json;
      switch (request.method) {
        case 'POST':
          if (request.uri.path.endsWith('/peers')) {
            request.response.write(
              jsonEncode(<String, Object?>{
                'id': 'dlbox',
                'name': '',
                'tags': <String>[],
                'group_id': 'lab',
                'revision': 51,
              }),
            );
          } else {
            request.response.write(
              jsonEncode(<String, Object?>{
                'username': 'bob',
                'permission': 'write',
                'group_id': 'lab',
                'created_at_ms': 40,
              }),
            );
          }
        case 'DELETE':
          request.response.statusCode = HttpStatus.noContent;
        case 'GET':
          request.response.write(
            jsonEncode(<String, Object?>{
              'books': <Object?>[
                <String, Object?>{
                  'owner': 'carol',
                  'permission': 'write',
                  'revision': 5,
                  'groups': <Object?>[
                    <String, Object?>{'id': 'lab', 'name': '实验室'},
                  ],
                  'scopes': <Object?>[
                    <String, Object?>{'permission': 'read'},
                    <String, Object?>{
                      'group_id': 'lab',
                      'permission': 'write',
                    },
                  ],
                  'peers': <Object?>[
                    <String, Object?>{'id': 'devbox', 'group_id': 'lab'},
                  ],
                },
              ],
            }),
          );
      }
      await request.response.close();
    });

    final AddressBookGrant grant = await ServerApi.shareAddressBook(
      settings,
      'account-token',
      'bob',
      permission: 'write',
      groupId: 'lab',
    );
    expect(grant.groupId, 'lab');
    expect(grant.wholeBook, isFalse);
    await ServerApi.unshareAddressBook(
      settings,
      'account-token',
      'bob',
      groupId: 'lab',
    );
    await ServerApi.shareSharedAddressBook(
      settings,
      'account-token',
      'carol',
      'erin',
      groupId: 'lab',
    );
    await ServerApi.unshareSharedAddressBook(
      settings,
      'account-token',
      'carol',
      'erin',
      groupId: '',
    );
    final AddressBookPeer added = await ServerApi.addSharedAddressBookPeer(
      settings,
      'account-token',
      'carol',
      'dlbox',
      groupId: 'lab',
    );
    expect(added.groupId, 'lab');
    final List<SharedAddressBook> books = await ServerApi.sharedAddressBooks(
      settings,
      'account-token',
    );
    final SharedAddressBook book = books.single;
    expect(book.permissionFor('lab'), 'write');
    expect(book.permissionFor(''), 'read');
    expect(book.permissionFor('work'), '');

    expect(requests, <String>[
      'POST /v1/addressbook/grants '
          '{"username":"bob","permission":"write","group_id":"lab"}',
      'DELETE /v1/addressbook/grants/bob?group_id=lab ',
      'POST /v1/addressbook/shared/carol/grants '
          '{"username":"erin","permission":"read","group_id":"lab"}',
      'DELETE /v1/addressbook/shared/carol/grants/erin?group_id ',
      'POST /v1/addressbook/shared/carol/peers '
          '{"id":"dlbox","name":"","tags":[],"group_id":"lab"}',
      'GET /v1/addressbook/shared ',
    ]);
  });

  test('shared writes surface a permission failure', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.forbidden;
      await request.response.close();
    });

    await expectLater(
      ServerApi.updateSharedAddressBookPeer(
        settings,
        'account-token',
        'carol',
        'sharedbox',
        name: '会议室',
      ),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 403)
            .having(
              (ServerApiException e) => e.message,
              'message',
              '共享地址簿权限不足',
            ),
      ),
    );
    await expectLater(
      ServerApi.addSharedAddressBookPeer(
        settings,
        'account-token',
        'carol',
        'dlbox',
        name: '下载机',
      ),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 403)
            .having(
              (ServerApiException e) => e.message,
              'message',
              '共享地址簿权限不足',
            ),
      ),
    );
    await expectLater(
      ServerApi.sharedAddressBookGrants(settings, 'account-token', 'carol'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 403)
            .having(
              (ServerApiException e) => e.message,
              'message',
              '共享地址簿权限不足',
            ),
      ),
    );
  });

  test('logout accepts 204 and surfaces a missing account store', () async {
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls++;
      request.response.statusCode = calls == 1
          ? HttpStatus.noContent
          : HttpStatus.serviceUnavailable;
      await request.response.close();
    });

    await ServerApi.logout(settings, 'account-token');
    expect(calls, 1);
    await expectLater(
      ServerApi.addressBook(settings, 'account-token'),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'statusCode', 503)
            .having(
              (ServerApiException e) => e.message,
              'message',
              contains('未启用账号'),
            ),
      ),
    );
  });

  test('connection reports carry the peer, timing and account token', () async {
    String? method;
    Uri? requested;
    String? authorization;
    Map<String, Object?>? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body =
          jsonDecode(await utf8.decoder.bind(request).join())
              as Map<String, Object?>;
      request.response.statusCode = HttpStatus.created;
      request.response.headers.contentType = ContentType.json;
      request.response.write(jsonEncode(<String, Object?>{'id': 7}));
      await request.response.close();
    });

    final DateTime started = DateTime.utc(2026, 10, 25, 9);
    final DateTime ended = started.add(const Duration(minutes: 2));
    await ServerApi.reportConnection(
      settings,
      'account-token',
      peerId: ' devbox ',
      startedAt: started,
      endedAt: ended,
      detail: 'closed',
    );

    expect(method, 'POST');
    expect(requested?.path, '/v1/account/connections');
    expect(authorization, 'Bearer account-token');
    expect(body?['peer_id'], 'devbox');
    expect(body?['started_at_ms'], started.millisecondsSinceEpoch);
    expect(body?['ended_at_ms'], ended.millisecondsSinceEpoch);
    expect(body?['success'], isTrue);
    expect(body?['detail'], 'closed');
  });

  test('an empty connection peer issues no request', () async {
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls += 1;
      await request.response.close();
    });

    await ServerApi.reportConnection(
      settings,
      'account-token',
      peerId: '  ',
      startedAt: DateTime.utc(2026, 10, 25, 9),
      endedAt: DateTime.utc(2026, 10, 25, 9, 1),
    );
    expect(calls, 0);
  });

  test('file transfer reports carry direction, count and account token',
      () async {
    String? method;
    Uri? requested;
    String? authorization;
    Map<String, Object?>? body;
    server.listen((HttpRequest request) async {
      method = request.method;
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      body =
          jsonDecode(await utf8.decoder.bind(request).join())
              as Map<String, Object?>;
      request.response.statusCode = HttpStatus.created;
      request.response.headers.contentType = ContentType.json;
      request.response.write(jsonEncode(<String, Object?>{'id': 9}));
      await request.response.close();
    });

    final DateTime occurred = DateTime.utc(2026, 10, 25, 12);
    await ServerApi.reportFileTransfers(
      settings,
      'account-token',
      peerId: ' devbox ',
      direction: 'download',
      name: ' photos.zip ',
      size: 900,
      files: 3,
      success: false,
      detail: 'disk full',
      occurredAt: occurred,
    );

    expect(method, 'POST');
    expect(requested?.path, '/v1/account/file-transfers');
    expect(authorization, 'Bearer account-token');
    expect(body?['peer_id'], 'devbox');
    expect(body?['direction'], 'download');
    expect(body?['name'], 'photos.zip');
    expect(body?['size'], 900);
    expect(body?['files'], 3);
    expect(body?['success'], isFalse);
    expect(body?['detail'], 'disk full');
    expect(body?['occurred_at_ms'], occurred.millisecondsSinceEpoch);
  });

  test('an empty peer or unknown direction issues no file request', () async {
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls += 1;
      await request.response.close();
    });

    await ServerApi.reportFileTransfers(
      settings,
      'account-token',
      peerId: '  ',
      direction: 'upload',
    );
    await ServerApi.reportFileTransfers(
      settings,
      'account-token',
      peerId: 'devbox',
      direction: 'viewer',
    );
    expect(calls, 0);
  });
}
