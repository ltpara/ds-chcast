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

  test('sendFileTo names the browsed directory and returns the queue size',
      () async {
    calls.clear();
    mock((MethodCall call) async => 2);
    expect(await ViewerBridge.sendFileTo('photos'), 2);
    expect(calls.single.method, 'sendFileTo');
    expect(calls.single.arguments, <String, Object?>{'directory': 'photos'});
  });

  test('sendFileTo keeps null when the picker was cancelled', () async {
    calls.clear();
    mock((MethodCall call) async => null);
    expect(await ViewerBridge.sendFileTo(''), isNull);
    expect(calls.single.method, 'sendFileTo');
    expect(calls.single.arguments, <String, Object?>{'directory': ''});
  });

  test('sendFilesTo carries dropped paths and the browsed directory',
      () async {
    calls.clear();
    mock((MethodCall call) async => 1);
    expect(await ViewerBridge.sendFilesTo(<String>['C:\\a.txt'], 'photos'), 1);
    expect(calls.single.method, 'sendFileAt');
    expect(calls.single.arguments, <String, Object?>{
      'paths': <String>['C:\\a.txt'],
      'directory': 'photos',
    });
  });
}
