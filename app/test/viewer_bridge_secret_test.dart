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

  test('protectSecret passes the plaintext and returns the blob', () async {
    calls.clear();
    mock((MethodCall call) async => 'QkJC');
    expect(await ViewerBridge.protectSecret('{"token":"t"}'), 'QkJC');
    expect(calls.single.method, 'protectSecret');
    expect(calls.single.arguments, '{"token":"t"}');
  });

  test('protectSecret reports an empty native result', () async {
    calls.clear();
    mock((MethodCall call) async => null);
    await expectLater(
      ViewerBridge.protectSecret('x'),
      throwsA(
        isA<PlatformException>()
            .having((PlatformException e) => e.code, 'code', 'dpapi'),
      ),
    );
  });

  test('unprotectSecret returns the plaintext', () async {
    calls.clear();
    mock((MethodCall call) async => '{"token":"t"}');
    expect(await ViewerBridge.unprotectSecret('QkJC'), '{"token":"t"}');
    expect(calls.single.method, 'unprotectSecret');
    expect(calls.single.arguments, 'QkJC');
  });

  test('unprotectSecret surfaces a DPAPI failure', () async {
    calls.clear();
    mock((MethodCall call) async => throw PlatformException(code: 'dpapi'));
    await expectLater(
      ViewerBridge.unprotectSecret('QkJC'),
      throwsA(
        isA<PlatformException>()
            .having((PlatformException e) => e.code, 'code', 'dpapi'),
      ),
    );
  });
}
