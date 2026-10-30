import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/security_key.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  // flutter_test installs an HttpOverrides that answers every request with
  // 400; these tests need a real loopback server for the account routes.
  HttpOverrides.global = null;
  const MethodChannel channel = MethodChannel('chast/viewer');
  final List<MethodCall> calls = <MethodCall>[];

  setUp(() {
    calls.clear();
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  void mock(Future<Object?> Function(MethodCall call) handler) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall call) {
          calls.add(call);
          return handler(call);
        });
  }

  test('available reads the native answer', () async {
    mock((MethodCall call) async => true);
    expect(await SecurityKey().available(), isTrue);
    expect(calls.single.method, 'securityKeyAvailable');
  });

  test('available reports a missing bridge as not supported', () async {
    await expectLater(
      SecurityKey().available(),
      throwsA(
        isA<SecurityKeyException>().having(
          (SecurityKeyException error) => error.code,
          'code',
          'not_supported',
        ),
      ),
    );
  });

  test('makeCredential forwards base64url fields and the default origin',
      () async {
    mock((MethodCall call) async => <String, Object?>{
          'credentialId': 'Y3JlZA',
          'clientDataJSON': 'Y2xpZW50',
          'attestationObject': 'YXR0',
        });
    final SecurityKeyMakeResult result = await SecurityKey().makeCredential(
      rpId: 'chast.example.com',
      userId: 'dXNlcg',
      username: 'alice',
      challenge: 'Y2hhbGxlbmdl',
      excludeCredentials: <String>['b2xk'],
      timeoutMs: 90000,
    );
    final MethodCall call = calls.single;
    expect(call.method, 'securityKeyMakeCredential');
    expect(call.arguments, <String, Object?>{
      'rpId': 'chast.example.com',
      'origin': 'https://chast.example.com',
      'userId': 'dXNlcg',
      'username': 'alice',
      'displayName': 'alice',
      'challenge': 'Y2hhbGxlbmdl',
      'credentials': <String>['b2xk'],
      'timeoutMs': 90000,
    });
    expect(result.credentialId, 'Y3JlZA');
    expect(result.clientDataJSON, 'Y2xpZW50');
    expect(result.attestationObject, 'YXR0');
  });

  test('getAssertion forwards the allow list and parses the signature',
      () async {
    mock((MethodCall call) async => <String, Object?>{
          'credentialId': 'Y3JlZA',
          'clientDataJSON': 'Y2xpZW50',
          'authenticatorData': 'YXV0aA',
          'signature': 'c2ln',
        });
    final SecurityKeyAssertion assertion = await SecurityKey().getAssertion(
      rpId: 'chast.example.com',
      challenge: 'Y2hhbGxlbmdl',
      allowCredentials: <String>['Y3JlZA'],
    );
    final MethodCall call = calls.single;
    expect(call.method, 'securityKeyGetAssertion');
    expect((call.arguments as Map<Object?, Object?>)['credentials'], <String>[
      'Y3JlZA',
    ]);
    expect(
      (call.arguments as Map<Object?, Object?>)['origin'],
      'https://chast.example.com',
    );
    expect(assertion.authenticatorData, 'YXV0aA');
    expect(assertion.signature, 'c2ln');
  });

  test('a cancelled ceremony surfaces the native code', () async {
    mock((MethodCall call) async {
      throw PlatformException(code: 'user_cancelled', message: 'cancelled');
    });
    await expectLater(
      SecurityKey().getAssertion(
        rpId: 'chast.example.com',
        challenge: 'Y2hhbGxlbmdl',
        allowCredentials: <String>['Y3JlZA'],
      ),
      throwsA(
        isA<SecurityKeyException>()
            .having(
              (SecurityKeyException error) => error.code,
              'code',
              'user_cancelled',
            )
            .having(
              (SecurityKeyException error) => error.message,
              'message',
              '已取消 Windows Hello 验证',
            ),
      ),
    );
  });

  test('loginWithSecurityKey starts, asserts and finishes the login', () async {
    late HttpServer server;
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    addTearDown(() => server.close(force: true));
    final ServerSettings settings = ServerSettings(
      url: 'http://127.0.0.1:${server.port}',
    );
    final List<String> paths = <String>[];
    final List<Object?> bodies = <Object?>[];
    server.listen((HttpRequest request) async {
      final String text = await utf8.decoder.bind(request).join();
      paths.add(request.uri.path);
      bodies.add(text.isEmpty ? null : jsonDecode(text));
      request.response.headers.contentType = ContentType.json;
      if (request.uri.path.endsWith('/begin')) {
        request.response.write(jsonEncode(<String, Object?>{
          'challenge': 'Y2hhbGxlbmdl',
          'rp_id': 'chast.example.com',
          'username': 'alice',
          'timeout_ms': 60000,
          'credentials': <Object?>[
            <String, Object?>{'id': 'Y3JlZA', 'name': '此电脑'},
          ],
        }));
      } else {
        request.response.write(jsonEncode(<String, Object?>{
          'token': 'acct-token',
          'expires_in_ms': 900000,
          'user': <String, Object?>{'username': 'alice'},
        }));
      }
      await request.response.close();
    });
    mock((MethodCall call) async {
      if (call.method == 'securityKeyAvailable') {
        return true;
      }
      return <String, Object?>{
        'credentialId': 'Y3JlZA',
        'clientDataJSON': 'Y2xpZW50',
        'authenticatorData': 'YXV0aA',
        'signature': 'c2ln',
      };
    });

    final AccountLoginResult result = await loginWithSecurityKey(
      settings,
      username: 'alice',
      password: 'secret',
      deviceName: '开发机',
    );

    expect(calls.map((MethodCall call) => call.method), <String>[
      'securityKeyAvailable',
      'securityKeyGetAssertion',
    ]);
    final MethodCall assertion = calls.last;
    expect(
      (assertion.arguments as Map<Object?, Object?>)['challenge'],
      'Y2hhbGxlbmdl',
    );
    expect(paths, <String>[
      '/v1/account/login/webauthn/begin',
      '/v1/account/login/webauthn/finish',
    ]);
    expect(bodies.first, <String, Object?>{
      'username': 'alice',
      'password': 'secret',
    });
    expect((bodies.last as Map<Object?, Object?>)['device_name'], '开发机');
    expect(result.token, 'acct-token');
    expect(result.username, 'alice');
  });

  test('loginWithSecurityKeyOnly starts, asserts and finishes without a password',
      () async {
    late HttpServer server;
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    addTearDown(() => server.close(force: true));
    final ServerSettings settings = ServerSettings(
      url: 'http://127.0.0.1:${server.port}',
    );
    final List<String> paths = <String>[];
    final List<Object?> bodies = <Object?>[];
    server.listen((HttpRequest request) async {
      final String text = await utf8.decoder.bind(request).join();
      paths.add(request.uri.path);
      bodies.add(text.isEmpty ? null : jsonDecode(text));
      request.response.headers.contentType = ContentType.json;
      if (request.uri.path.endsWith('/begin')) {
        request.response.write(jsonEncode(<String, Object?>{
          'challenge': 'Y2hhbGxlbmdl',
          'rp_id': 'chast.example.com',
          'username': 'alice',
          'timeout_ms': 60000,
          'credentials': <Object?>[
            <String, Object?>{'id': 'Y3JlZA', 'name': '此电脑'},
          ],
        }));
      } else {
        request.response.write(jsonEncode(<String, Object?>{
          'token': 'acct-token',
          'expires_in_ms': 900000,
          'user': <String, Object?>{'username': 'alice'},
        }));
      }
      await request.response.close();
    });
    mock((MethodCall call) async {
      if (call.method == 'securityKeyAvailable') {
        return true;
      }
      return <String, Object?>{
        'credentialId': 'Y3JlZA',
        'clientDataJSON': 'Y2xpZW50',
        'authenticatorData': 'YXV0aA',
        'signature': 'c2ln',
      };
    });

    final AccountLoginResult result = await loginWithSecurityKeyOnly(
      settings,
      username: 'alice',
      deviceName: '开发机',
    );

    expect(calls.map((MethodCall call) => call.method), <String>[
      'securityKeyAvailable',
      'securityKeyGetAssertion',
    ]);
    expect(paths, <String>[
      '/v1/account/login/webauthn/passwordless/begin',
      '/v1/account/login/webauthn/passwordless/finish',
    ]);
    expect(bodies.first, <String, Object?>{'username': 'alice'});
    expect((bodies.last as Map<Object?, Object?>)['device_name'], '开发机');
    expect(result.token, 'acct-token');
    expect(result.username, 'alice');
  });

  test('loginWithSecurityKeyOnly reports an account without keys', () async {
    late HttpServer server;
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    addTearDown(() => server.close(force: true));
    final ServerSettings settings = ServerSettings(
      url: 'http://127.0.0.1:${server.port}',
    );
    server.listen((HttpRequest request) async {
      await request.drain<void>();
      request.response.statusCode = 202;
      request.response.headers.contentType = ContentType.json;
      request.response.write(jsonEncode(<String, Object?>{'status': 'accepted'}));
      await request.response.close();
    });
    mock((MethodCall call) async => true);

    await expectLater(
      loginWithSecurityKeyOnly(settings, username: 'alice'),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException error) => error.message,
          'message',
          '该账号未注册安全密钥',
        ),
      ),
    );
    expect(calls, isEmpty);
  });

  test('loginWithSecurityKey reports an account without keys', () async {
    late HttpServer server;
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    addTearDown(() => server.close(force: true));
    final ServerSettings settings = ServerSettings(
      url: 'http://127.0.0.1:${server.port}',
    );
    server.listen((HttpRequest request) async {
      await request.drain<void>();
      request.response.statusCode = 202;
      request.response.headers.contentType = ContentType.json;
      request.response.write(jsonEncode(<String, Object?>{'status': 'accepted'}));
      await request.response.close();
    });
    mock((MethodCall call) async => true);

    await expectLater(
      loginWithSecurityKey(
        settings,
        username: 'alice',
        password: 'secret',
      ),
      throwsA(
        isA<ServerApiException>().having(
          (ServerApiException error) => error.message,
          'message',
          '该账号未注册安全密钥',
        ),
      ),
    );
    expect(calls, isEmpty);
  });
}
