import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/session_window.dart';
import 'package:chast_viewer/target.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');
  final List<MethodCall> calls = <MethodCall>[];

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
    calls.clear();
  });

  void mock(Future<Object?> Function(MethodCall call) handler) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall call) {
          calls.add(call);
          return handler(call);
        });
  }

  SessionWindowRequest request() => SessionWindowRequest(
        target: const ConnectTarget(
          '',
          0,
          token: 'token-1',
          server: 'https://server.example:8443',
          hostId: 'host-1',
          displayName: 'Desk',
          mode: TunnelMode.relay,
          serverKey: 'server-key',
        ),
        account: AccountSession(
          server: 'https://server.example:8443',
          username: 'alice',
          token: 'account-token',
          expiresAt: DateTime.fromMillisecondsSinceEpoch(
            1893456000000,
            isUtc: true,
          ),
        ),
        settings: const ServerSettings(
          url: 'https://server.example:8443',
          mode: TunnelMode.relay,
          key: 'service-key',
          proxy: 'socks5://127.0.0.1:1080',
          uploadRecordings: true,
          useWebSocket: true,
        ),
        filesOnly: true,
        sessionMode: 'desktop',
        rdpPort: 3389,
        display: 1,
      );

  test('arguments round trip every connection field', () {
    final SessionWindowRequest original = request();
    final SessionWindowRequest? decoded =
        SessionWindowRequest.fromArguments(<String>[
      '--session-window',
      original.encode(),
    ]);

    expect(decoded, isNotNull);
    final SessionWindowRequest copy = decoded!;
    expect(copy.target.host, '');
    expect(copy.target.port, 0);
    expect(copy.target.token, 'token-1');
    expect(copy.target.server, 'https://server.example:8443');
    expect(copy.target.hostId, 'host-1');
    expect(copy.target.displayName, 'Desk');
    expect(copy.target.mode, TunnelMode.relay);
    expect(copy.target.serverKey, 'server-key');
    expect(copy.account?.username, 'alice');
    expect(copy.account?.token, 'account-token');
    expect(copy.settings.url, 'https://server.example:8443');
    expect(copy.settings.mode, TunnelMode.relay);
    expect(copy.settings.key, 'service-key');
    expect(copy.settings.proxy, 'socks5://127.0.0.1:1080');
    expect(copy.settings.uploadRecordings, isTrue);
    expect(copy.settings.useWebSocket, isTrue);
    expect(copy.filesOnly, isTrue);
    expect(copy.sessionMode, 'desktop');
    expect(copy.rdpPort, 3389);
    expect(copy.display, 1);
  });

  test('args without the session flag are ignored', () {
    expect(SessionWindowRequest.fromArguments(const <String>[]), isNull);
    expect(
      SessionWindowRequest.fromArguments(<String>['--connect', '1.2.3.4']),
      isNull,
    );
    expect(
      SessionWindowRequest.fromArguments(
          <String>['--session-window', '{not json']),
      isNull,
    );
  });

  test('open hands the encoded request to the runner', () async {
    mock((MethodCall call) async => true);
    expect(await request().open(), isTrue);
    expect(calls.single.method, 'openSessionWindow');
    expect(calls.single.arguments, request().encode());
  });

  test('open falls back when no runner answers', () async {
    expect(await request().open(), isFalse);
  });

  test('open falls back when the runner refuses', () async {
    mock((MethodCall call) async => false);
    expect(await request().open(), isFalse);
  });

  test('close asks the runner to close this window', () async {
    mock((MethodCall call) async => null);
    await SessionWindowRequest.close();
    expect(calls.single.method, 'closeSessionWindow');
  });
}
