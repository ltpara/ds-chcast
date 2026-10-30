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
              'sha256': '0f343b0931126a20f133d67c2b018a3c',
              'size': 73400320,
              'signature': 'MEUCIQ-test-signature',
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
      expect(batch.software?.sha256, '0f343b0931126a20f133d67c2b018a3c');
      expect(batch.software?.size, 73400320);
      expect(batch.software?.installable, isTrue);
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

  test('lookupBatch keeps an unsigned offer without marking it installable', () async {
    server.listen((HttpRequest request) async {
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        '{"hosts":[],"software":{"version":"2.1.0",'
        '"url":"https://downloads.example.com/chast-2.1.0.exe",'
        '"sha256":"","size":0,"signature":""}}',
      );
      await request.response.close();
    });

    final BatchLookupResult batch = await ServerApi.lookupBatch(
      settings,
      <String>['devbox'],
    );
    expect(batch.software?.version, '2.1.0');
    expect(batch.software?.installable, isFalse);
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

    final AccountLoginResult result = await ServerApi.refresh(
      settings,
      ' old-refresh ',
    );
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

  test(
    'changeRecoveryEmail posts the address with the current password',
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
        request.response.headers.contentType = ContentType.json;
        request.response.write(
          jsonEncode(<String, Object?>{'email': 'alice@example.com'}),
        );
        await request.response.close();
      });

      final String stored = await ServerApi.changeRecoveryEmail(
        settings,
        'account-token',
        email: ' alice@example.com ',
        password: 'secret123',
      );
      expect(method, 'POST');
      expect(requested?.path, '/v1/account/email');
      expect(authorization, 'Bearer account-token');
      expect(body, <String, Object?>{
        'email': 'alice@example.com',
        'password': 'secret123',
      });
      expect(stored, 'alice@example.com');
    },
  );

  test('changeRecoveryEmail reports a duplicate address', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.conflict;
      request.response.write('{"error":"email is already in use"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.changeRecoveryEmail(
        settings,
        'account-token',
        email: 'taken@example.com',
        password: 'secret123',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '该邮箱已被其他账号使用',
        ),
      ),
    );
  });

  test(
    'requestPasswordRecovery posts a name, accepts 202 and sends no key',
    () async {
      Uri? requested;
      String? authorization;
      Object? body;
      server.listen((HttpRequest request) async {
        requested = request.uri;
        authorization = request.headers.value(HttpHeaders.authorizationHeader);
        body = jsonDecode(await utf8.decoder.bind(request).join());
        request.response.statusCode = HttpStatus.accepted;
        request.response.headers.contentType = ContentType.json;
        request.response.write('{"status":"accepted"}');
        await request.response.close();
      });

      await ServerApi.requestPasswordRecovery(
        settings.copyWith(key: 'server-key'),
        username: ' alice ',
      );
      expect(requested?.path, '/v1/account/recover');
      expect(authorization, isNull);
      expect(body, <String, Object?>{'username': 'alice'});
    },
  );

  test('requestPasswordRecovery reports a server without mail', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.serviceUnavailable;
      request.response.write('{"error":"email recovery is not configured"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.requestPasswordRecovery(settings, username: 'alice'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '连接服务未配置账号或邮件找回',
        ),
      ),
    );
  });

  test('completePasswordRecovery posts the token and new password', () async {
    Uri? requested;
    Object? body;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{'username': 'alice'}),
      );
      await request.response.close();
    });

    final String username = await ServerApi.completePasswordRecovery(
      settings,
      token: ' reset-token ',
      password: 'new secret 1',
    );
    expect(requested?.path, '/v1/account/recover/complete');
    expect(body, <String, Object?>{
      'token': 'reset-token',
      'password': 'new secret 1',
    });
    expect(username, 'alice');
  });

  test('completePasswordRecovery reports an expired token', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.badRequest;
      request.response.write(
        '{"error":"password reset token is invalid or expired"}',
      );
      await request.response.close();
    });

    await expectLater(
      ServerApi.completePasswordRecovery(
        settings,
        token: 'expired',
        password: 'new secret 1',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '重置令牌无效、已过期或密码不符合要求',
        ),
      ),
    );
  });

  test(
    'changeRecoveryPhone posts the number with the current password',
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
        request.response.headers.contentType = ContentType.json;
        request.response.write(
          jsonEncode(<String, Object?>{'phone': '+86 138 0013 8000'}),
        );
        await request.response.close();
      });

      final String stored = await ServerApi.changeRecoveryPhone(
        settings,
        'account-token',
        phone: ' +86 138 0013 8000 ',
        password: 'secret123',
      );
      expect(method, 'POST');
      expect(requested?.path, '/v1/account/phone');
      expect(authorization, 'Bearer account-token');
      expect(body, <String, Object?>{
        'phone': '+86 138 0013 8000',
        'password': 'secret123',
      });
      expect(stored, '+86 138 0013 8000');
    },
  );

  test('changeRecoveryPhone validates the number before calling', () async {
    int calls = 0;
    server.listen((HttpRequest request) async {
      calls += 1;
      request.response.close();
    });

    await expectLater(
      ServerApi.changeRecoveryPhone(
        settings,
        'account-token',
        phone: '12345',
        password: 'secret123',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '手机号需为 6–15 位数字',
        ),
      ),
    );
    expect(calls, 0);
  });

  test(
    'changeRecoveryPhone reports a duplicate number and a wrong password',
    () async {
      int calls = 0;
      server.listen((HttpRequest request) async {
        calls += 1;
        request.response.statusCode = calls == 1
            ? HttpStatus.conflict
            : HttpStatus.forbidden;
        request.response.write('{"error":"phone"}');
        await request.response.close();
      });

      await expectLater(
        ServerApi.changeRecoveryPhone(
          settings,
          'account-token',
          phone: '13800138000',
          password: 'secret123',
        ),
        throwsA(
          isA<ServerApiException>().having(
            (ServerApiException e) => e.message,
            'message',
            '该手机号已被其他账号使用',
          ),
        ),
      );
      await expectLater(
        ServerApi.changeRecoveryPhone(
          settings,
          'account-token',
          phone: '13800138000',
          password: 'wrong-password',
        ),
        throwsA(
          isA<ServerApiException>().having(
            (ServerApiException e) => e.message,
            'message',
            '当前密码不正确',
          ),
        ),
      );
      expect(calls, 2);
    },
  );

  test(
    'requestSMSRecovery posts a name and accepts 202 without a key',
    () async {
      Uri? requested;
      String? authorization;
      Object? body;
      server.listen((HttpRequest request) async {
        requested = request.uri;
        authorization = request.headers.value(HttpHeaders.authorizationHeader);
        body = jsonDecode(await utf8.decoder.bind(request).join());
        request.response.statusCode = HttpStatus.accepted;
        request.response.headers.contentType = ContentType.json;
        request.response.write('{"status":"accepted"}');
        await request.response.close();
      });

      await ServerApi.requestSMSRecovery(
        settings.copyWith(key: 'server-key'),
        username: ' alice ',
      );
      expect(requested?.path, '/v1/account/recover/sms');
      expect(authorization, isNull);
      expect(body, <String, Object?>{'username': 'alice'});
    },
  );

  test('requestSMSRecovery reports a server without SMS', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.serviceUnavailable;
      request.response.write('{"error":"sms recovery is not configured"}');
      await request.response.close();
    });

    await expectLater(
      ServerApi.requestSMSRecovery(settings, username: 'alice'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '连接服务未配置账号或短信找回',
        ),
      ),
    );
  });

  test('completeSMSRecovery posts the name, code and new password', () async {
    Uri? requested;
    Object? body;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      body = jsonDecode(await utf8.decoder.bind(request).join());
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{'username': 'alice'}),
      );
      await request.response.close();
    });

    final String username = await ServerApi.completeSMSRecovery(
      settings,
      username: ' alice ',
      code: ' 1234-5678 ',
      password: 'new secret 1',
    );
    expect(requested?.path, '/v1/account/recover/sms/complete');
    expect(body, <String, Object?>{
      'username': 'alice',
      'code': '12345678',
      'password': 'new secret 1',
    });
    expect(username, 'alice');
  });

  test('completeSMSRecovery reports an invalid code', () async {
    server.listen((HttpRequest request) async {
      request.response.statusCode = HttpStatus.badRequest;
      request.response.write(
        '{"error":"sms reset code is invalid or expired"}',
      );
      await request.response.close();
    });

    await expectLater(
      ServerApi.completeSMSRecovery(
        settings,
        username: 'alice',
        code: '00000000',
        password: 'new secret 1',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '短信验证码无效、已过期或密码不符合要求',
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
            .having(
              (ServerApiException e) => e.message,
              'message',
              '验证码或恢复码不正确',
            ),
      ),
    );
    expect(calls, 2);
  });

  test(
    'login sends a recovery code instead of the authenticator code',
    () async {
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
    },
  );

  test('login sends an SMS code only when no other factor is set', () async {
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
      smsCode: ' 1234 5678 ',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'sms_code': '12345678',
    });

    await ServerApi.login(
      settings,
      username: 'alice',
      password: 'secret123',
      totpCode: '123456',
      smsCode: '12345678',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'totp_code': '123456',
    });

    await expectLater(
      ServerApi.login(
        settings,
        username: 'alice',
        password: 'secret123',
        smsCode: '1234',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '短信验证码需为 8 位数字',
        ),
      ),
    );
  });

  test('requestLoginSMS posts credentials and reports rejections', () async {
    final List<Object?> bodies = <Object?>[];
    final List<String> paths = <String>[];
    int status = HttpStatus.accepted;
    server.listen((HttpRequest request) async {
      paths.add(request.uri.path);
      bodies.add(jsonDecode(await utf8.decoder.bind(request).join()));
      request.response.statusCode = status;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          if (status == HttpStatus.accepted) 'status': 'accepted',
        }),
      );
      await request.response.close();
    });

    await ServerApi.requestLoginSMS(
      settings,
      username: ' alice ',
      password: 'secret123',
    );
    expect(paths, <String>['/v1/account/login/sms']);
    expect(bodies.single, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
    });

    status = HttpStatus.unauthorized;
    await expectLater(
      ServerApi.requestLoginSMS(
        settings,
        username: 'alice',
        password: 'wrong horse',
      ),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.message, 'message', '用户名或密码不正确')
            .having((ServerApiException e) => e.statusCode, 'status', 401),
      ),
    );

    status = HttpStatus.forbidden;
    await expectLater(
      ServerApi.requestLoginSMS(
        settings,
        username: 'alice',
        password: 'secret123',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '账号已被封禁',
        ),
      ),
    );

    status = HttpStatus.tooManyRequests;
    await expectLater(
      ServerApi.requestLoginSMS(
        settings,
        username: 'alice',
        password: 'secret123',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '请求过于频繁，请稍后再试',
        ),
      ),
    );

    await expectLater(
      ServerApi.requestLoginSMS(settings, username: '', password: 'secret123'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '请输入用户名和密码',
        ),
      ),
    );
  });

  test('login sends an email code only when no other factor is set', () async {
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
      emailCode: ' 1234 5678 ',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'email_code': '12345678',
    });

    await ServerApi.login(
      settings,
      username: 'alice',
      password: 'secret123',
      smsCode: '12345678',
      emailCode: '87654321',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'sms_code': '12345678',
    });

    await ServerApi.login(
      settings,
      username: 'alice',
      password: 'secret123',
      totpCode: '123456',
      emailCode: '87654321',
    );
    expect(body, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
      'totp_code': '123456',
    });

    await expectLater(
      ServerApi.login(
        settings,
        username: 'alice',
        password: 'secret123',
        emailCode: '1234',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '邮件验证码需为 8 位数字',
        ),
      ),
    );
  });

  test('requestLoginEmail posts credentials and reports rejections', () async {
    final List<Object?> bodies = <Object?>[];
    final List<String> paths = <String>[];
    int status = HttpStatus.accepted;
    server.listen((HttpRequest request) async {
      paths.add(request.uri.path);
      bodies.add(jsonDecode(await utf8.decoder.bind(request).join()));
      request.response.statusCode = status;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          if (status == HttpStatus.accepted) 'status': 'accepted',
        }),
      );
      await request.response.close();
    });

    await ServerApi.requestLoginEmail(
      settings,
      username: ' alice ',
      password: 'secret123',
    );
    expect(paths, <String>['/v1/account/login/email']);
    expect(bodies.single, <String, Object?>{
      'username': 'alice',
      'password': 'secret123',
    });

    status = HttpStatus.unauthorized;
    await expectLater(
      ServerApi.requestLoginEmail(
        settings,
        username: 'alice',
        password: 'wrong horse',
      ),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.message, 'message', '用户名或密码不正确')
            .having((ServerApiException e) => e.statusCode, 'status', 401),
      ),
    );

    status = HttpStatus.forbidden;
    await expectLater(
      ServerApi.requestLoginEmail(
        settings,
        username: 'alice',
        password: 'secret123',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '账号已被封禁',
        ),
      ),
    );

    status = HttpStatus.serviceUnavailable;
    await expectLater(
      ServerApi.requestLoginEmail(
        settings,
        username: 'alice',
        password: 'secret123',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '连接服务未配置账号或邮件登录',
        ),
      ),
    );

    await expectLater(
      ServerApi.requestLoginEmail(
        settings,
        username: '',
        password: 'secret123',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          '请输入用户名和密码',
        ),
      ),
    );
  });

  test(
    'accountInfo reads the two-factor flag and the recovery phone',
    () async {
      Uri? requested;
      String? authorization;
      server.listen((HttpRequest request) async {
        requested = request.uri;
        authorization = request.headers.value(HttpHeaders.authorizationHeader);
        request.response.headers.contentType = ContentType.json;
        request.response.write(
          jsonEncode(<String, Object?>{
            'user': <String, Object?>{
              'id': 'u1',
              'username': 'alice',
              'email': 'alice@example.com',
              'phone': '+86 138 0013 8000',
            },
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
      expect(info.email, 'alice@example.com');
      expect(info.phone, '+86 138 0013 8000');
      expect(info.twoFactor, isTrue);
      expect(info.recoveryCodes, 4);
    },
  );

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
    expect(body, <String, Object?>{'password': 'secret123', 'code': '654321'});
    expect(codes, <String>['ABCD-EFGH-IJKL-MNOP', 'QRST-UVWX-YZ23-4567']);
  });

  test(
    'generateRecoveryCodes maps rejected proofs and disabled state',
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
    },
  );

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
    int deletes = 0;
    server.listen((HttpRequest request) async {
      final String body = await utf8.decoder.bind(request).join();
      requests.add('${request.method} ${request.uri.path} $body');
      request.response.headers.contentType = ContentType.json;
      if (request.method == 'PATCH') {
        request.response.write(
          jsonEncode(<String, Object?>{
            'id': 'work',
            'name': 'Work',
            'parent_id': 'team',
            'position': 2,
            'updated_at_ms': 1761300000000,
            'revision': 6,
          }),
        );
      } else if (deletes++ == 0) {
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
    expect(group.parentId, 'team');
    expect(group.position, 2);
    expect(group.revision, 6);

    final AddressBookGroup placed = await ServerApi.updateAddressBookGroup(
      settings,
      'account-token',
      'work',
      null,
      parentId: ' team ',
      position: 3,
    );
    expect(
      requests.last,
      'PATCH /v1/addressbook/groups/work {"parent_id":"team","position":3}',
    );
    expect(placed.parentId, 'team');

    await expectLater(
      ServerApi.updateAddressBookGroup(
        settings,
        'account-token',
        'work',
        null,
        position: -1,
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('排序位置'),
        ),
      ),
    );
    await expectLater(
      ServerApi.updateAddressBookGroup(settings, 'account-token', 'work', null),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('至少需要一项'),
        ),
      ),
    );

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
        jsonEncode(<String, Object?>{
          'id': 'devbox',
          'group_id': 'work',
          'position': 3,
        }),
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

    body = null;
    final AddressBookPeer placed = await ServerApi.updateAddressBookPeer(
      settings,
      'account-token',
      'devbox',
      position: 3,
    );
    expect(body, <String, Object?>{'position': 3});
    expect(placed.position, 3);
    await expectLater(
      ServerApi.updateAddressBookPeer(
        settings,
        'account-token',
        'devbox',
        position: -1,
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException e) => e.message,
          'message',
          contains('排序位置'),
        ),
      ),
    );
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
                    <String, Object?>{'group_id': 'lab', 'permission': 'write'},
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

  test(
    'file transfer reports carry direction, count and account token',
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
    },
  );

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

  test('listRecordings parses the archive and its limits', () async {
    String? authorization;
    Uri? requested;
    server.listen((HttpRequest request) async {
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      requested = request.uri;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'recordings': <Object?>[
            <String, Object?>{
              'id': 'a' * 32,
              'name': '会话录制.mp4',
              'peer_id': 'devbox',
              'size': 2048,
              'sha256': 'AB' * 32,
              'duration_ms': 60000,
              'started_at_ms': 1761400000000,
              'created_at_ms': 1761400060000,
            },
          ],
          'usage_bytes': 2048,
          'quota_bytes': 8192,
          'max_upload_bytes': 4096,
        }),
      );
      await request.response.close();
    });

    final RecordingArchive archive = await ServerApi.listRecordings(
      settings,
      'account-token',
    );
    expect(requested?.path, '/v1/account/recordings');
    expect(authorization, 'Bearer account-token');
    expect(archive.recordings, hasLength(1));
    expect(archive.recordings.single.name, '会话录制.mp4');
    expect(archive.recordings.single.peerId, 'devbox');
    expect(archive.recordings.single.durationMs, 60000);
    expect(archive.recordings.single.createdAt?.toUtc().millisecondsSinceEpoch,
        1761400060000);
    expect(archive.usageBytes, 2048);
    expect(archive.quotaBytes, 8192);
    expect(archive.maxUploadBytes, 4096);
  });

  test('uploadRecording streams the file with encoded metadata', () async {
    final Directory dir = await Directory.systemTemp.createTemp(
      'chast-upload-',
    );
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}rec.mp4');
    final List<int> bytes = List<int>.generate(1024, (int index) => index % 251);
    await file.writeAsBytes(bytes);

    Uri? requested;
    String? authorization;
    ContentType? contentType;
    int? contentLength;
    List<int>? body;
    server.listen((HttpRequest request) async {
      requested = request.uri;
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      contentType = request.headers.contentType;
      contentLength = request.headers.contentLength;
      body = await request.fold<List<int>>(
        <int>[],
        (List<int> all, List<int> chunk) => all..addAll(chunk),
      );
      request.response.statusCode = HttpStatus.created;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'recording': <String, Object?>{
            'id': 'b' * 32,
            'name': '会话录制.mp4',
            'peer_id': 'devbox',
            'size': 1024,
            'sha256': 'CD' * 32,
            'duration_ms': 5000,
            'started_at_ms': 1761400000000,
            'created_at_ms': 1761400005000,
          },
          'usage_bytes': 1024,
          'quota_bytes': 8192,
        }),
      );
      await request.response.close();
    });

    int lastSent = 0;
    int lastTotal = 0;
    final RemoteRecording recorded = await ServerApi.uploadRecording(
      settings,
      'account-token',
      file: file,
      name: '会话录制.mp4',
      peerId: 'devbox',
      startedAtMs: 1761400000000,
      durationMs: 5000,
      onProgress: (int sent, int total) {
        lastSent = sent;
        lastTotal = total;
      },
    );

    expect(requested?.path, '/v1/account/recordings');
    expect(requested?.queryParameters['name'], '会话录制.mp4');
    expect(requested?.queryParameters['peer_id'], 'devbox');
    expect(requested?.queryParameters['started_ms'], '1761400000000');
    expect(requested?.queryParameters['duration_ms'], '5000');
    expect(authorization, 'Bearer account-token');
    expect(contentType?.mimeType, 'video/mp4');
    expect(contentLength, 1024);
    expect(body, bytes);
    expect(lastSent, 1024);
    expect(lastTotal, 1024);
    expect(recorded.id, 'b' * 32);
    expect(recorded.name, '会话录制.mp4');
  });

  test('uploadRecording maps a quota refusal to a readable reason', () async {
    final Directory dir = await Directory.systemTemp.createTemp(
      'chast-upload-',
    );
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}rec.mp4');
    await file.writeAsBytes(<int>[1, 2, 3, 4]);
    server.listen((HttpRequest request) async {
      await request.drain<void>();
      request.response.statusCode = HttpStatus.requestEntityTooLarge;
      request.response.headers.contentType = ContentType.json;
      request.response.write(
        jsonEncode(<String, Object?>{
          'error': 'account recording quota is exhausted',
        }),
      );
      await request.response.close();
    });

    await expectLater(
      ServerApi.uploadRecording(
        settings,
        'account-token',
        file: file,
      ),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'status', 413)
            .having((ServerApiException e) => e.message, 'message', '录制存储配额已用尽'),
      ),
    );
  });

  test('downloadRecording saves without overwriting and sends the token', () async {
    final Directory dir = await Directory.systemTemp.createTemp(
      'chast-download-',
    );
    addTearDown(() => dir.delete(recursive: true));
    String? authorization;
    server.listen((HttpRequest request) async {
      authorization = request.headers.value(HttpHeaders.authorizationHeader);
      request.response.headers.contentType = ContentType('video', 'mp4');
      request.response.add(<int>[0, 1, 2, 3, 4]);
      await request.response.close();
    });
    final RemoteRecording recording = RemoteRecording(
      id: 'c' * 32,
      name: r'..\会话/录制 2026.mp4',
      size: 5,
    );

    final File first = await ServerApi.downloadRecording(
      settings,
      'account-token',
      recording,
      directory: dir,
    );
    final File second = await ServerApi.downloadRecording(
      settings,
      'account-token',
      recording,
      directory: dir,
    );

    expect(authorization, 'Bearer account-token');
    expect(first.path, endsWith('会话_录制 2026.mp4'));
    expect(second.path, endsWith('会话_录制 2026 (1).mp4'));
    expect(await first.readAsBytes(), <int>[0, 1, 2, 3, 4]);
    expect(await second.readAsBytes(), <int>[0, 1, 2, 3, 4]);
  });

  test('deleteRecording accepts 204 and reports a missing recording', () async {
    final List<String> methods = <String>[];
    final List<String> paths = <String>[];
    bool missing = false;
    server.listen((HttpRequest request) async {
      methods.add(request.method);
      paths.add(request.uri.path);
      if (missing) {
        request.response.statusCode = HttpStatus.notFound;
        request.response.headers.contentType = ContentType.json;
        request.response.write(jsonEncode(<String, Object?>{'error': 'x'}));
      } else {
        request.response.statusCode = HttpStatus.noContent;
      }
      await request.response.close();
    });

    await ServerApi.deleteRecording(settings, 'account-token', 'd' * 32);
    expect(methods, <String>['DELETE']);
    expect(paths, <String>['/v1/account/recordings/${'d' * 32}']);

    missing = true;
    await expectLater(
      ServerApi.deleteRecording(settings, 'account-token', 'd' * 32),
      throwsA(
        isA<ServerApiException>()
            .having((ServerApiException e) => e.statusCode, 'status', 404)
            .having((ServerApiException e) => e.message, 'message', '录制不存在或已被删除'),
      ),
    );
  });
}
