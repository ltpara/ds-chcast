import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/security_key_dialog.dart';
import 'package:chast_viewer/security_key.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  const ServerSettings settings = ServerSettings(url: 'http://127.0.0.1:18645');
  final AccountSession account = AccountSession(
    server: settings.url,
    username: 'alice',
    token: 'account-token',
    expiresAt: DateTime.utc(2030),
  );

  Future<void> open(WidgetTester tester, SecurityKeyDialog dialog) async {
    tester.view.physicalSize = const Size(1400, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Builder(
            builder: (BuildContext context) {
              return TextButton(
                onPressed: () => showDialog<void>(
                  context: context,
                  builder: (BuildContext context) => dialog,
                ),
                child: const Text('打开'),
              );
            },
          ),
        ),
      ),
    );
    await tester.tap(find.text('打开'));
    await tester.pumpAndSettle();
  }

  testWidgets('lists registered keys and the platform status', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      SecurityKeyDialog(
        server: settings,
        account: account,
        probeAvailability: () async => true,
        loadCredentials: (ServerSettings server, String token) async {
          expect(token, 'account-token');
          return <WebAuthnCredential>[
            WebAuthnCredential(
              id: 'Y3JlZA',
              name: '此电脑',
              createdAtMs: 1700000000000,
              lastUsedAtMs: 1700000100000,
            ),
            const WebAuthnCredential(id: 'b2xk', name: '备用密钥'),
          ];
        },
      ),
    );

    expect(
      tester.widget<Text>(find.byKey(const Key('security-key-status'))).data,
      '此设备支持 Windows Hello',
    );
    expect(find.text('此电脑'), findsOneWidget);
    expect(find.text('备用密钥'), findsOneWidget);
    expect(find.byKey(const Key('security-key-row-1')), findsOneWidget);
    expect(find.byKey(const Key('security-key-empty')), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('registers a key with the password and shows the new row', (
    WidgetTester tester,
  ) async {
    final List<String> calls = <String>[];
    await open(
      tester,
      SecurityKeyDialog(
        server: settings,
        account: account,
        probeAvailability: () async => true,
        loadCredentials: (ServerSettings server, String token) async {
          return const <WebAuthnCredential>[];
        },
        startRegistration:
            (
              ServerSettings server,
              String token, {
              required String password,
            }) async {
          calls.add('start|$token|$password');
          return const WebAuthnRegisterOptions(
            challenge: 'Y2hhbGxlbmdl',
            rpId: 'chast.example.com',
            userId: 'dXNlcg',
            username: 'alice',
            displayName: 'alice',
            timeoutMs: 60000,
            excludeCredentials: <WebAuthnCredential>[
              WebAuthnCredential(id: 'b2xk'),
            ],
          );
        },
        makeCredential:
            ({
              required String rpId,
              required String userId,
              required String username,
              required String displayName,
              required String challenge,
              required List<String> excludeCredentials,
              required int timeoutMs,
            }) async {
          calls.add('make|$rpId|$userId|$challenge|${excludeCredentials.join(',')}|$timeoutMs');
          return const SecurityKeyMakeResult(
            credentialId: 'bmV3',
            clientDataJSON: 'Y2xpZW50',
            attestationObject: 'YXR0',
          );
        },
        finishRegistration:
            (
              ServerSettings server,
              String token, {
              required String name,
              required String clientDataJSON,
              required String attestationObject,
            }) async {
          calls.add('finish|$token|$name|$clientDataJSON|$attestationObject');
          return const WebAuthnCredential(id: 'bmV3', name: '此电脑');
        },
      ),
    );

    await tester.enterText(
      find.byKey(const Key('security-key-password')),
      'secret',
    );
    await tester.enterText(
      find.byKey(const Key('security-key-name')),
      '此电脑',
    );
    await tester.tap(find.byKey(const Key('security-key-register')));
    await tester.pumpAndSettle();

    expect(calls, <String>[
      'start|account-token|secret',
      'make|chast.example.com|dXNlcg|Y2hhbGxlbmdl|b2xk|60000',
      'finish|account-token|此电脑|Y2xpZW50|YXR0',
    ]);
    expect(
      tester.widget<Text>(find.byKey(const Key('security-key-message'))).data,
      '安全密钥已添加',
    );
    expect(find.byKey(const Key('security-key-row-0')), findsOneWidget);
    expect(find.text('此电脑'), findsOneWidget);
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('security-key-password')))
          .controller
          ?.text,
      isEmpty,
    );

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('removing a key needs the password and drops the row', (
    WidgetTester tester,
  ) async {
    final List<String> removed = <String>[];
    await open(
      tester,
      SecurityKeyDialog(
        server: settings,
        account: account,
        probeAvailability: () async => false,
        loadCredentials: (ServerSettings server, String token) async {
          return const <WebAuthnCredential>[
            WebAuthnCredential(id: 'Y3JlZA', name: '此电脑'),
          ];
        },
        removeCredential:
            (
              ServerSettings server,
              String token, {
              required String password,
              required String credentialId,
            }) async {
          removed.add('$token|$password|$credentialId');
        },
      ),
    );

    expect(
      tester.widget<Text>(find.byKey(const Key('security-key-status'))).data,
      '此设备未配置 Windows Hello',
    );
    await tester.tap(find.byKey(const Key('security-key-remove-0')));
    await tester.pumpAndSettle();
    expect(removed, isEmpty);
    expect(find.text('删除安全密钥需要当前密码'), findsOneWidget);

    await tester.enterText(
      find.byKey(const Key('security-key-password')),
      'secret',
    );
    await tester.tap(find.byKey(const Key('security-key-remove-0')));
    await tester.pumpAndSettle();

    expect(removed, <String>['account-token|secret|Y3JlZA']);
    expect(find.byKey(const Key('security-key-row-0')), findsNothing);
    expect(find.byKey(const Key('security-key-empty')), findsOneWidget);
    expect(
      tester.widget<Text>(find.byKey(const Key('security-key-message'))).data,
      '安全密钥已删除',
    );

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a failed load can be retried', (WidgetTester tester) async {
    int attempts = 0;
    await open(
      tester,
      SecurityKeyDialog(
        server: settings,
        account: account,
        probeAvailability: () async => true,
        loadCredentials: (ServerSettings server, String token) async {
          attempts += 1;
          if (attempts == 1) {
            throw const ServerApiException('连接服务未启用安全密钥');
          }
          return const <WebAuthnCredential>[];
        },
      ),
    );

    expect(find.text('连接服务未启用安全密钥'), findsOneWidget);
    expect(find.byKey(const Key('security-key-register')), findsNothing);

    await tester.tap(find.byKey(const Key('security-key-retry')));
    await tester.pumpAndSettle();

    expect(attempts, 2);
    expect(find.byKey(const Key('security-key-empty')), findsOneWidget);
    expect(find.byKey(const Key('security-key-register')), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });
}
