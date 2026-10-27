import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  late HttpServer server;
  late ServerSettings settings;
  late List<Map<String, Object?>> requests;

  setUp(() async {
    requests = <Map<String, Object?>>[];
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    settings = ServerSettings(url: 'http://127.0.0.1:${server.port}');
  });

  tearDown(() async {
    await server.close(force: true);
  });

  /// Answers the next request with [status] and [body]. The returned future
  /// completes once the request has been served, so tests can await it after
  /// starting the API call.
  Future<void> serve(int status, Object? body) {
    final Completer<void> done = Completer<void>();
    server.listen((HttpRequest request) async {
      final String text = await utf8.decoder.bind(request).join();
      requests.add(<String, Object?>{
        'path': request.uri.path,
        'method': request.method,
        'authorization': request.headers.value(HttpHeaders.authorizationHeader),
        'body': text.isEmpty ? null : jsonDecode(text),
      });
      request.response.statusCode = status;
      if (body != null) {
        request.response.headers.contentType = ContentType.json;
        request.response.write(jsonEncode(body));
      }
      await request.response.close();
      if (!done.isCompleted) {
        done.complete();
      }
    });
    return done.future;
  }

  test('loginWebAuthnBegin posts the password and reads the challenge',
      () async {
    final Future<void> answered = serve(200, <String, Object?>{
      'challenge': 'Y2hhbGxlbmdl',
      'rp_id': 'chast.example.com',
      'username': 'alice',
      'timeout_ms': 60000,
      'credentials': <Object?>[
        <String, Object?>{
          'id': 'Y3JlZA',
          'alg': -7,
          'name': '此电脑',
          'created_at_ms': 1700000000000,
        },
      ],
    });
    final WebAuthnLoginOptions? options = await ServerApi.loginWebAuthnBegin(
      settings,
      username: ' alice ',
      password: 'secret',
    );
    await answered;
    expect(requests.single['path'], '/v1/account/login/webauthn/begin');
    expect(requests.single['method'], 'POST');
    expect(requests.single['authorization'], isNull);
    expect(requests.single['body'], <String, Object?>{
      'username': 'alice',
      'password': 'secret',
    });
    expect(options, isNotNull);
    expect(options!.challenge, 'Y2hhbGxlbmdl');
    expect(options.rpId, 'chast.example.com');
    expect(options.timeoutMs, 60000);
    expect(options.credentials.single.id, 'Y3JlZA');
    expect(options.credentials.single.name, '此电脑');
  });

  test('loginWebAuthnBegin returns null when no key is registered', () async {
    final Future<void> answered = serve(202, <String, Object?>{
      'status': 'accepted',
    });
    final WebAuthnLoginOptions? options = await ServerApi.loginWebAuthnBegin(
      settings,
      username: 'alice',
      password: 'secret',
    );
    await answered;
    expect(options, isNull);
  });

  test('loginWebAuthnBegin rejects a wrong password', () async {
    final Future<void> answered = serve(401, <String, Object?>{'error': 'denied'});
    await expectLater(
      ServerApi.loginWebAuthnBegin(
        settings,
        username: 'alice',
        password: 'wrong',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException error) => error.message,
          'message',
          '用户名或密码不正确',
        ),
      ),
    );
    await answered;
  });

  test('loginWebAuthnFinish posts the assertion and parses the session',
      () async {
    final Future<void> answered = serve(200, <String, Object?>{
      'token': 'acct-token',
      'expires_in_ms': 900000,
      'refresh_token': 'refresh-token',
      'refresh_expires_in_ms': 86400000,
      'user': <String, Object?>{'username': 'alice'},
    });
    final AccountLoginResult result = await ServerApi.loginWebAuthnFinish(
      settings,
      username: 'alice',
      credentialId: 'Y3JlZA',
      clientDataJSON: 'Y2xpZW50',
      authenticatorData: 'YXV0aA',
      signature: 'c2ln',
      deviceName: '开发机',
    );
    await answered;
    expect(requests.single['path'], '/v1/account/login/webauthn/finish');
    expect(requests.single['body'], <String, Object?>{
      'username': 'alice',
      'credential_id': 'Y3JlZA',
      'client_data_json': 'Y2xpZW50',
      'authenticator_data': 'YXV0aA',
      'signature': 'c2ln',
      'device_name': '开发机',
    });
    expect(result.token, 'acct-token');
    expect(result.username, 'alice');
    expect(result.refreshToken, 'refresh-token');
    expect(result.refreshExpiresInMs, 86400000);
  });

  test('registration begin and finish carry the token and name', () async {
    final Future<void> beginAnswered = serve(200, <String, Object?>{
      'challenge': 'Y2hhbGxlbmdl',
      'rp_id': 'chast.example.com',
      'user_id': 'dXNlcg',
      'username': 'alice',
      'display_name': 'alice',
      'timeout_ms': 90000,
      'exclude_credentials': <Object?>[
        <String, Object?>{'id': 'b2xk', 'name': '旧密钥'},
      ],
    });
    final WebAuthnRegisterOptions options =
        await ServerApi.webauthnRegisterBegin(
      settings,
      'tok',
      password: 'secret',
    );
    await beginAnswered;
    expect(requests.single['authorization'], 'Bearer tok');
    expect(requests.single['body'], <String, Object?>{'password': 'secret'});
    expect(options.userId, 'dXNlcg');
    expect(options.timeoutMs, 90000);
    expect(options.excludeCredentials.single.id, 'b2xk');

    await server.close(force: true);
    requests.clear();
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    settings = ServerSettings(url: 'http://127.0.0.1:${server.port}');
    final Future<void> finishAnswered = serve(200, <String, Object?>{
      'id': 'bmV3',
      'alg': -7,
      'name': '此电脑',
      'created_at_ms': 1700000000000,
    });
    final WebAuthnCredential stored = await ServerApi.webauthnRegisterFinish(
      settings,
      'tok',
      name: '此电脑',
      clientDataJSON: 'Y2xpZW50',
      attestationObject: 'YXR0',
    );
    await finishAnswered;
    expect(requests.single['path'], '/v1/account/webauthn/register/finish');
    expect(requests.single['body'], <String, Object?>{
      'name': '此电脑',
      'client_data_json': 'Y2xpZW50',
      'attestation_object': 'YXR0',
    });
    expect(stored.id, 'bmV3');
    expect(stored.name, '此电脑');
  });

  test('webauthnCredentials and webauthnRemove use the account token',
      () async {
    final Future<void> listAnswered = serve(200, <String, Object?>{
      'credentials': <Object?>[
        <String, Object?>{'id': 'Y3JlZA', 'name': '此电脑'},
      ],
    });
    final List<WebAuthnCredential> credentials =
        await ServerApi.webauthnCredentials(settings, 'tok');
    await listAnswered;
    expect(requests.single['method'], 'GET');
    expect(requests.single['path'], '/v1/account/webauthn/credentials');
    expect(requests.single['authorization'], 'Bearer tok');
    expect(credentials.single.id, 'Y3JlZA');

    await server.close(force: true);
    requests.clear();
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    settings = ServerSettings(url: 'http://127.0.0.1:${server.port}');
    final Future<void> removeAnswered = serve(204, null);
    await ServerApi.webauthnRemove(
      settings,
      'tok',
      password: 'secret',
      credentialId: 'Y3JlZA',
    );
    await removeAnswered;
    expect(requests.single['path'], '/v1/account/webauthn/credentials/remove');
    expect(requests.single['body'], <String, Object?>{
      'password': 'secret',
      'credential_id': 'Y3JlZA',
    });
  });

  test('a rejected registration maps the password error', () async {
    final Future<void> answered = serve(403, <String, Object?>{'error': 'denied'});
    await expectLater(
      ServerApi.webauthnRegisterBegin(settings, 'tok', password: 'wrong'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException error) => error.message,
          'message',
          '当前密码不正确',
        ),
      ),
    );
    await answered;
  });
}
