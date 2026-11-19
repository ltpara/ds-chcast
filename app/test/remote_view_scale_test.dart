import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/widgets/remote_view.dart';

void main() {
  testWidgets(
    'zoomed view keeps scale and input stable after a fullscreen resize',
    (tester) async {
      final Size originalPhysicalSize = tester.view.physicalSize;
      final double originalDevicePixelRatio = tester.view.devicePixelRatio;
      addTearDown(() {
        tester.view.devicePixelRatio = originalDevicePixelRatio;
        tester.view.physicalSize = originalPhysicalSize;
      });
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);

      final List<double> scales = <double>[];
      final List<Offset> clicks = <Offset>[];
      Widget buildView() => MaterialApp(
        home: RemoteView(
          textureId: 1,
          videoWidth: 800,
          videoHeight: 600,
          sourceWidth: 1600,
          sourceHeight: 1200,
          fit: ViewFit.adaptive,
          zoomPercent: 150,
          captureInput: true,
          onScaleChanged: scales.add,
          onButton: (Offset position, int _, bool down) {
            if (down) clicks.add(position);
          },
        ),
      );
      await tester.pumpWidget(buildView());
      await tester.pump();
      expect(scales.last, 0.75);

      Future<void> clickFrameCenter() async {
        final TestGesture mouse = await tester.createGesture(
          kind: PointerDeviceKind.mouse,
          buttons: kPrimaryMouseButton,
        );
        await mouse.down(tester.getCenter(find.byType(Texture)));
        await mouse.up();
      }

      await clickFrameCenter();
      expect(clicks.last, const Offset(800, 600));

      tester.view.physicalSize = const Size(1600, 1200);
      await tester.pump();
      await tester.pump();
      expect(scales.last, 1.5);
      await clickFrameCenter();
      expect(clicks.last, const Offset(800, 600));
    },
  );

  testWidgets('rotated stream input uses post-rotation source dimensions', (
    tester,
  ) async {
    final Size originalPhysicalSize = tester.view.physicalSize;
    final double originalDevicePixelRatio = tester.view.devicePixelRatio;
    addTearDown(() {
      tester.view.devicePixelRatio = originalDevicePixelRatio;
      tester.view.physicalSize = originalPhysicalSize;
    });
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(640, 800);

    final List<Offset> clicks = <Offset>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Center(
          child: SizedBox(
            width: 480,
            height: 640,
            child: RemoteView(
              textureId: 1,
              videoWidth: 480,
              videoHeight: 640,
              sourceWidth: 960,
              sourceHeight: 1280,
              fit: ViewFit.adaptive,
              captureInput: true,
              onButton: (Offset position, int _, bool down) {
                if (down) clicks.add(position);
              },
            ),
          ),
        ),
      ),
    );
    await tester.pump();

    final TestGesture mouse = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kPrimaryMouseButton,
    );
    await mouse.down(tester.getCenter(find.byType(Texture)));
    await mouse.up();
    expect(clicks, <Offset>[const Offset(480, 640)]);
  });

  testWidgets('zoomed view routes normal and Shift wheel separately', (
    tester,
  ) async {
    final List<Offset> remoteScrolls = <Offset>[];
    await tester.pumpWidget(
      MaterialApp(
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
              zoomPercent: 150,
              captureInput: true,
              onScroll: (Offset position, Offset _) =>
                  remoteScrolls.add(position),
            ),
          ),
        ),
      ),
    );
    await tester.pump();

    final position =
        tester.getRect(find.byType(RemoteView)).topLeft +
        const Offset(200, 150);
    Future<void> sendWheel() => tester.sendEventToBinding(
      PointerScrollEvent(
        device: 1,
        position: position,
        scrollDelta: const Offset(0, 120),
      ),
    );

    final vertical = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byType(SingleChildScrollView).first,
            matching: find.byType(Scrollable),
          )
          .first,
    );
    final horizontal = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byType(SingleChildScrollView).at(1),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    expect(vertical.position.maxScrollExtent, greaterThan(0));
    expect(horizontal.position.maxScrollExtent, greaterThan(0));

    await sendWheel();
    await tester.pump();
    expect(remoteScrolls, hasLength(1));
    expect(vertical.position.pixels, 0);

    await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
    await sendWheel();
    await tester.pump();
    await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
    expect(remoteScrolls, hasLength(1));
    expect(vertical.position.pixels, 0);
    expect(horizontal.position.pixels, greaterThan(0));
  });

  testWidgets('custom view zoom updates cursor scale and input coordinates', (
    tester,
  ) async {
    final List<Offset> buttons = <Offset>[];
    final List<double> scales = <double>[];
    await tester.pumpWidget(
      MaterialApp(
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
              zoomPercent: 150,
              captureInput: true,
              onScaleChanged: scales.add,
              onButton: (Offset position, int _, bool _) {
                buttons.add(position);
              },
            ),
          ),
        ),
      ),
    );
    await tester.pump();

    expect(scales, <double>[0.375]);
    final Offset origin = tester.getTopLeft(find.byType(Texture));
    final TestGesture mouse = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kPrimaryMouseButton,
    );
    await mouse.down(origin + const Offset(300, 225));
    expect(buttons, <Offset>[const Offset(800, 600)]);
    await mouse.up();
  });
}
