import 'package:chast_viewer/viewer_bridge.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');
  final TestDefaultBinaryMessenger messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  testWidgets('installation key signs the supplied base64url challenge', (
    WidgetTester tester,
  ) async {
    MethodCall? received;
    messenger.setMockMethodCallHandler(channel, (MethodCall call) async {
      received = call;
      return 'signature';
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));

    expect(
      await ViewerBridge.signLoginDeviceChallenge('Y2hhbGxlbmdl'),
      'signature',
    );
    expect(received?.method, 'signLoginDeviceChallenge');
    expect(received?.arguments, 'Y2hhbGxlbmdl');
  });
}
