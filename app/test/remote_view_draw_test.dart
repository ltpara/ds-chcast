import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

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

Widget _view(_Events events) => MaterialApp(
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
        drawEnabled: true,
        onHover: (_, _) => events.hover++,
        onMove: (_, _) => events.move++,
        onButton: (_, _, _) => events.button++,
        onScroll: (_, _) => events.scroll++,
        onGesture: (_, _, _, _) => events.gesture++,
        onDevicePointer: (_, _) => events.devicePointer++,
        onDeviceCancel: (_, _) => events.deviceCancel++,
        onDrawStart: events.drawStart.add,
        onDrawMove: events.drawMove.add,
        onDrawEnd: events.drawEnd.add,
        onDrawCancel: () => events.drawCancel++,
      ),
    ),
  ),
);

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
}
