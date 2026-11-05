import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/remote_view.dart';

typedef GestureSample = (Offset, bool, int, int);

void main() {
  final gestures = <GestureSample>[];
  final buttons = <(Offset, int, bool)>[];
  final moves = <Offset>[];
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
          onButton: (p, b, d) => buttons.add((p, b, d)),
          onMove: (p, _) => moves.add(p),
        ),
      ),
    ),
  );

  setUp(() {
    gestures.clear();
    buttons.clear();
    moves.clear();
  });

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
      expect(buttons, isEmpty);
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

  testWidgets(
    'two touch contacts pan together without a click or spurious pinch',
    (tester) async {
      await tester.pumpWidget(view());
      final anchor = tester.getCenter(find.byType(Texture));
      final a = await tester.startGesture(
        anchor - const Offset(50, 0),
        pointer: 11,
      );
      final b = await tester.startGesture(
        anchor + const Offset(50, 0),
        pointer: 12,
      );
      expect(buttons, isEmpty);
      await a.moveBy(const Offset(0, 53));
      await b.moveBy(const Offset(0, 53));
      await tester.pump();
      expect(gestures.single.$2, isFalse);
      expect(gestures.single.$3, 0);
      expect(gestures.single.$4, 120);
      await a.up();
      await b.moveBy(const Offset(20, 0));
      await b.up();
      expect(buttons, isEmpty);
      expect(moves, isEmpty);
    },
  );

  testWidgets('touch pinch does not also scroll; cancel does not click', (
    tester,
  ) async {
    await tester.pumpWidget(view());
    final anchor = tester.getCenter(find.byType(Texture));
    final a = await tester.startGesture(
      anchor - const Offset(50, 0),
      pointer: 21,
    );
    final b = await tester.startGesture(
      anchor + const Offset(50, 0),
      pointer: 22,
    );
    await a.moveBy(const Offset(-10, 0));
    await b.moveBy(const Offset(10, 0));
    await tester.pump();
    expect(gestures.single, (const Offset(800, 600), true, 0, 120));
    await a.cancel();
    await b.cancel();
    final tap = await tester.startGesture(anchor, pointer: 23);
    await tap.cancel();
    expect(buttons, isEmpty);
  });

  testWidgets(
    'single touch clicks at source coordinates and drag releases on cancel',
    (tester) async {
      await tester.pumpWidget(view());
      final anchor = tester.getCenter(find.byType(Texture));
      final tap = await tester.startGesture(anchor);
      await tap.up();
      expect(buttons, [
        (const Offset(800, 600), ViewerMouse.left, true),
        (const Offset(800, 600), ViewerMouse.left, false),
      ]);
      buttons.clear();
      final drag = await tester.startGesture(anchor, pointer: 31);
      await drag.moveBy(const Offset(25, 0));
      expect(buttons.single.$3, isTrue);
      expect(moves.single, const Offset(900, 600));
      await drag.cancel();
      expect(buttons.last.$3, isFalse);
    },
  );

  test('gesture bridge preserves the physical modifier bitmap', () async {
    const channel = MethodChannel('chast/viewer');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'sendInput');
      expect(call.arguments, {
        'kind': 3,
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
