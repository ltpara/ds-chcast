import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/annotation_ink.dart';
import 'package:chast_viewer/widgets/remote_view.dart';

class _Events {
  final List<Offset> drawStart = <Offset>[];
  final List<Offset> drawMove = <Offset>[];
  final List<Offset> drawEnd = <Offset>[];
  int drawCancel = 0;
  int hover = 0;
  int move = 0;
  int button = 0;
  int pointer = 0;
  int devicePointer = 0;
  int deviceCancel = 0;
  int scroll = 0;
  int gesture = 0;

  int get remoteInputCalls =>
      hover +
      move +
      button +
      pointer +
      devicePointer +
      deviceCancel +
      scroll +
      gesture;
}

Widget _view(_Events events, {ViewerAnnotationInk? ink, bool drawEnabled = true}) =>
    MaterialApp(
      // The debug ribbon is red too and would pollute the ink pixel count.
      debugShowCheckedModeBanner: false,
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
            captureInput: true,
            drawEnabled: drawEnabled,
            ink: ink,
            onHover: (_, _) => events.hover++,
            onMove: (_, _) => events.move++,
            onButton: (_, _, _) => events.button++,
            onScroll: (_, _) => events.scroll++,
            onGesture: (_, _, _, _) => events.gesture++,
            onDevicePointer: (_, _) => events.devicePointer++,
            onDeviceCancel: (_, _) => events.deviceCancel++,
            onDrawStart: (Offset p) {
              events.drawStart.add(p);
              ink?.begin(0, p);
            },
            onDrawMove: (Offset p) {
              events.drawMove.add(p);
              ink?.move(p);
            },
            onDrawEnd: (Offset p) {
              events.drawEnd.add(p);
              ink?.end();
            },
            onDrawCancel: () {
              events.drawCancel++;
              ink?.end();
            },
          ),
        ),
      ),
    );

/// Counts pixels carrying the ink colour (a warm red) in the capture.
Future<int> _inkPixels(WidgetTester tester, Finder boundary) async {
  final RenderRepaintBoundary render = tester.renderObject<RenderRepaintBoundary>(
    boundary,
  );
  // Rasterisation runs on the engine, outside the fake test clock.
  final ui.Image? image = await tester.runAsync<ui.Image>(
    () => render.toImage(),
  );
  try {
    final ByteData? bytes = await tester.runAsync<ByteData?>(
      () => image!.toByteData(format: ui.ImageByteFormat.rawRgba),
    );
    final Uint8List data = bytes!.buffer.asUint8List();
    int count = 0;
    for (int i = 0; i + 3 < data.length; i += 4) {
      final int r = data[i];
      final int g = data[i + 1];
      final int b = data[i + 2];
      if (r > g + 40 && r > b + 40) count++;
    }
    return count;
  } finally {
    image?.dispose();
  }
}

void main() {
  testWidgets('drawing bypasses every remote input path', (tester) async {
    final _Events events = _Events();
    await tester.pumpWidget(_view(events));
    final Offset origin = tester.getTopLeft(find.byType(Texture));
    final TestGesture gesture = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kPrimaryMouseButton,
    );

    await gesture.down(origin + const Offset(10, 20));
    await gesture.moveTo(origin + const Offset(110, 120));
    await gesture.up();
    await tester.pump();

    expect(events.drawStart, <Offset>[const Offset(40, 80)]);
    expect(events.drawMove, <Offset>[
      const Offset(440, 480),
      const Offset(440, 480),
    ]);
    expect(events.drawEnd, <Offset>[const Offset(440, 480)]);
    expect(events.drawCancel, 0);
    expect(events.remoteInputCalls, 0);
  });

  testWidgets('touch drawing also never reaches the device pointer path', (
    tester,
  ) async {
    final _Events events = _Events();
    await tester.pumpWidget(_view(events));
    final Offset origin = tester.getTopLeft(find.byType(Texture));
    final TestGesture gesture = await tester.createGesture(
      kind: PointerDeviceKind.touch,
    );

    await gesture.down(origin + const Offset(10, 10));
    await gesture.moveTo(origin + const Offset(20, 30));
    await gesture.up();
    await tester.pump();

    expect(events.drawStart, hasLength(1));
    expect(events.drawMove, hasLength(2));
    expect(events.drawEnd, hasLength(1));
    expect(events.remoteInputCalls, 0);
  });

  testWidgets('cancelling a draw ends it without releasing remote buttons', (
    tester,
  ) async {
    final _Events events = _Events();
    await tester.pumpWidget(_view(events));
    final Offset origin = tester.getTopLeft(find.byType(Texture));
    final TestGesture gesture = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kPrimaryMouseButton,
    );

    await gesture.down(origin + const Offset(10, 10));
    await gesture.cancel();
    await tester.pump();

    expect(events.drawStart, hasLength(1));
    expect(events.drawCancel, 1);
    expect(events.drawEnd, isEmpty);
    expect(events.remoteInputCalls, 0);
  });

  testWidgets('the viewer draws its own ink over the frame', (tester) async {
    final _Events events = _Events();
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);
    const Key shot = ValueKey('ink-shot');
    await tester.pumpWidget(
      RepaintBoundary(key: shot, child: _view(events, ink: ink)),
    );
    final Offset origin = tester.getTopLeft(find.byType(Texture));
    final TestGesture gesture = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kPrimaryMouseButton,
    );

    await gesture.down(origin + const Offset(10, 20));
    await gesture.moveTo(origin + const Offset(110, 120));
    await gesture.up();
    await tester.pump();

    expect(ink.strokes, hasLength(1));
    expect(
      ink.strokes.single.points,
      containsAllInOrder(<Offset>[
        const Offset(40, 80),
        const Offset(440, 480),
      ]),
    );
    expect(
      await _inkPixels(tester, find.byKey(shot)),
      greaterThan(0),
      reason: 'the host ink is capture-excluded, so the viewer needs its own',
    );
  });

  testWidgets('ink is hidden while annotation mode is off', (tester) async {
    final _Events events = _Events();
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);
    ink.begin(0, const Offset(100, 100));
    ink.move(const Offset(400, 400));
    ink.end();
    const Key shot = ValueKey('ink-hidden-shot');
    await tester.pumpWidget(
      RepaintBoundary(
        key: shot,
        child: _view(events, ink: ink, drawEnabled: false),
      ),
    );

    expect(await _inkPixels(tester, find.byKey(shot)), 0);
  });
}
