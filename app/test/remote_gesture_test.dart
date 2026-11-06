import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/remote_view.dart';

typedef GestureSample = (Offset, bool, int, int);

void main() {
  final gestures = <GestureSample>[];
  Widget view({bool enabled = true}) => MaterialApp(
    home: Center(
      child: SizedBox(
        width: 400,
        height: 300,
        child: RemoteView(
          textureId: 1,
          videoWidth: 800,
          videoHeight: 600,
          sourceWidth: 1600,
          sourceHeight: 1200,
          fit: ViewFit.adaptive,
          captureInput: enabled,
          onGesture: (p, z, x, y) => gestures.add((p, z, x, y)),
        ),
      ),
    ),
  );

  setUp(gestures.clear);

  testWidgets(
    'trackpad pan preserves both axes and captured desktop coordinates',
    (tester) async {
      await tester.pumpWidget(view());
      final anchor = tester.getCenter(find.byType(Texture));
      final trackpad = await tester.createGesture(
        kind: PointerDeviceKind.trackpad,
      );
      await trackpad.panZoomStart(anchor);
      await trackpad.panZoomUpdate(anchor, pan: const Offset(53, -53));
      expect(gestures, [(const Offset(800, 600), false, -120, -120)]);
      await trackpad.panZoomEnd();
    },
  );

  testWidgets('pinch accumulates small updates and stops after gesture end', (
    tester,
  ) async {
    await tester.pumpWidget(view());
    final anchor = tester.getCenter(find.byType(Texture));
    final trackpad = await tester.createGesture(
      kind: PointerDeviceKind.trackpad,
    );
    await trackpad.panZoomStart(anchor);
    await trackpad.panZoomUpdate(anchor, scale: 1.04);
    expect(gestures, isEmpty);
    await trackpad.panZoomUpdate(anchor, scale: 1.12, pan: const Offset(0, 20));
    expect(gestures.single, (const Offset(800, 600), true, 0, 120));
    await trackpad.panZoomEnd();
    // A new pan must have neither zoom state nor fractional zoom remainder.
    await trackpad.panZoomStart(anchor);
    await trackpad.panZoomUpdate(anchor, pan: const Offset(0, 53));
    expect(gestures.last, (const Offset(800, 600), false, 0, 120));
    await trackpad.panZoomEnd();
  });

  testWidgets('revoking control discards the in-progress gesture', (
    tester,
  ) async {
    await tester.pumpWidget(view());
    final anchor = tester.getCenter(find.byType(Texture));
    final trackpad = await tester.createGesture(
      kind: PointerDeviceKind.trackpad,
    );
    await trackpad.panZoomStart(anchor);
    await tester.pumpWidget(view(enabled: false));
    await trackpad.panZoomUpdate(anchor, scale: 2);
    await tester.pumpWidget(view());
    await trackpad.panZoomUpdate(anchor, scale: 3);
    await trackpad.panZoomEnd();
    expect(gestures, isEmpty);
  });

  test('gesture bridge preserves the physical modifier bitmap', () async {
    const channel = MethodChannel('chast/viewer');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'sendInput');
      expect(call.arguments, {
        'kind': 5,
        'action': 1,
        'buttons': 0,
        'x': 0,
        'y': -120,
        'modifiers': 2,
      });
      return true;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    expect(
      await ViewerBridge.sendGesture(zoom: true, y: -120, modifiers: 2),
      isTrue,
    );
  });
}
