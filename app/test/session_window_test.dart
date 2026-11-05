import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/session_window.dart';
import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');
  final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  const request = SessionWindowRequest(
    target: ConnectTarget('2001:db8::1', 4433, token: 'private-token',
      server: 'https://server.example', hostId: 'office', mode: TunnelMode.relay,
      serverKey: 'server-key', socks5: 'socks5://localhost:1080'),
    filesOnly: true, display: 1,
  );

  test('window launch retains endpoint, access and display selection', () {
    final result = SessionWindowRequest.fromArguments(['--session-window', request.encode()]);
    expect(result, isNotNull);
    expect(result!.target.host, request.target.host);
    expect(result.target.token, 'private-token');
    expect(result.target.socks5, request.target.socks5);
    expect(result.target.mode, TunnelMode.relay);
    expect(result.display, 1);
    expect(result.filesOnly, isTrue);
    expect(SessionWindowRequest.fromArguments(['--session-window', '{']), isNull);
    expect(SessionWindowRequest.fromArguments(['--connect', request.encode()]), isNull);
  });

  test('new window uses the in-process bridge and propagates native refusal', () async {
    messenger.setMockMethodCallHandler(channel, (MethodCall call) async {
      expect(call.method, 'openSessionWindow');
      expect(jsonDecode(call.arguments as String)['display'], 1);
      return true;
    });
    expect(await request.open(), isTrue);
    messenger.setMockMethodCallHandler(channel, (_) async {
      throw PlatformException(code: 'window_open_failed');
    });
    await expectLater(request.open(), throwsA(isA<PlatformException>()));
  });

  test('native close waits for session cleanup and retains the file drop handler', () async {
    bool closed = false;
    List<String>? dropped;
    messenger.setMockMethodCallHandler(channel, (_) async => null);
    ViewerBridge.setDropHandler((List<String> paths) => dropped = paths);
    await ViewerBridge.setWindowCloseHandler(() async { closed = true; });
    Future<void> send(MethodCall call) async {
      await messenger.handlePlatformMessage('chast/viewer',
          const StandardMethodCodec().encodeMethodCall(call), (_) {});
    }
    await send(const MethodCall('droppedFiles', <String>['report.txt']));
    expect(dropped, ['report.txt']);
    await send(const MethodCall('windowCloseRequested'));
    expect(closed, isTrue);
    ViewerBridge.setDropHandler(null);
    await ViewerBridge.setWindowCloseHandler(null);
  });
}
