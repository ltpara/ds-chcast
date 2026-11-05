import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');
  final List<MethodCall> calls = <MethodCall>[];

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

  test('status carries the OS-login answer', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'inputEnabled': true,
      'permissions': ViewerPermission.input | ViewerPermission.osLogin,
      'osLoginSupported': true,
      'osLoginCode': ViewerHostActionCode.unsupported.code,
      'osLoginError': 'the host is not at a logon or lock screen',
      'osLoginUpdates': 3,
    });
    expect(status.osLoginSupported, isTrue);
    expect(status.osLoginCode, ViewerHostActionCode.unsupported.code);
    expect(
      status.osLoginError,
      'the host is not at a logon or lock screen',
    );
    expect(status.osLoginUpdates, 3);
    expect(status.acceptsOsLogin, isTrue);
  });

  test('OS login defaults to unavailable', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.osLoginSupported, isFalse);
    expect(status.osLoginCode, 0);
    expect(status.osLoginError, isEmpty);
    expect(status.osLoginUpdates, 0);
    expect(status.acceptsOsLogin, isFalse);
  });

  test('OS login needs the published grant, not just a permission bit', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'inputEnabled': true,
      'permissions': ViewerPermission.input | ViewerPermission.osLogin,
    });
    expect(status.acceptsOsLogin, isFalse);
  });

  test('OS login stays unavailable until the stream is live', () {
    final ViewerStatus connecting = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'handshaking',
      'inputEnabled': true,
      'permissions': ViewerPermission.input | ViewerPermission.osLogin,
      'osLoginSupported': true,
    });
    expect(connecting.acceptsOsLogin, isFalse);
  });

  test('requestOsLogin passes the password once and reports the queue', () async {
    calls.clear();
    mock((MethodCall call) async => true);
    expect(await ViewerBridge.requestOsLogin('host password'), isTrue);

    calls.clear();
    mock((MethodCall call) async => null);
    expect(await ViewerBridge.requestOsLogin('host password'), isFalse);

    expect(calls.single.method, 'requestOsLogin');
    expect(calls.single.arguments, 'host password');
  });
}
