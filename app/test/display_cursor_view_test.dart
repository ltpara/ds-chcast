import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/display_stream_controller.dart';
import 'package:chast_viewer/widgets/display_stream_view.dart';
import 'package:chast_viewer/widgets/remote_view.dart';

void main() {
  testWidgets(
    'cursor uses source scale, retains its sprite and never intercepts input',
    (tester) async {
      const channel = MethodChannel('chast/viewer');
      var x = -10, visible = true, shape = 5;
      var bytes = Uint8List.fromList(List.filled(20 * 20 * 4, 255));
      final controller = DisplayStreamController(
        register: () async => 10,
        output: (_) async => 1000,
        unregister: (_) async {},
      );
      addTearDown(() async {
        await controller.close();
        controller.dispose();
        tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
          channel,
          null,
        );
      });
      tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(channel, (
        call,
      ) async {
        if (call.method == 'displayStreams') {
          return {
            'active': true,
            'focused': 7,
            'streams': [
              {
                'streamId': 7,
                'displayIndex': 1,
                'width': 640,
                'height': 480,
                'sourceWidth': 1280,
                'sourceHeight': 960,
                'originX': -1280,
                'originY': -100,
                'published': 1,
                'ready': true,
                'bound': true,
              },
            ],
          };
        }
        if (call.method == 'takeDisplayCursors') {
          return [
            {
              'streamId': 7,
              'visible': visible,
              'x': x,
              'y': 40,
              'width': 20,
              'height': 20,
              'hotspotX': 5,
              'hotspotY': 6,
              'shapeId': shape,
              'bgra': bytes,
            },
          ];
        }
        return true;
      });
      await tester.runAsync(controller.refresh);
      final texture = controller.textures.single;
      final initialImage = texture.cursor;
      expect(initialImage, isNotNull);
      var focus = 0;
      Future<void> draw() => tester.pumpWidget(
        MaterialApp(
          home: Center(
            child: SizedBox(
              width: 320,
              height: 240,
              child: RemoteView(
                textureId: 10,
                videoWidth: 640,
                videoHeight: 480,
                sourceWidth: 1280,
                sourceHeight: 960,
                fit: ViewFit.adaptive,
                onFocusRequested: () {
                  focus++;
                },
                overlayBuilder: (size) => displayCursorOverlay(texture, size),
              ),
            ),
          ),
        ),
      );
      await draw();
      final origin = tester.getTopLeft(find.byType(Texture));
      expect(
        tester.getTopLeft(find.byType(RawImage)),
        origin + const Offset(-2.5, 10),
      );
      expect(tester.getSize(find.byType(RawImage)), const Size(5, 5));
      await tester.tapAt(origin + const Offset(1, 12));
      expect(
        focus,
        1,
        reason: 'view-only cursor cannot intercept focus selection',
      );
      bytes = Uint8List(0);
      x = 80;
      await tester.runAsync(() async {
        for (var i = 0; i < 100 && texture.pointer!.x != 80; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
      });
      expect(
        texture.pointer!.x,
        80,
        reason: 'cursor polling does not wait for the slower status refresh',
      );
      await draw();
      expect(texture.cursor, same(initialImage));
      expect(
        tester.getTopLeft(find.byType(RawImage)),
        origin + const Offset(20, 10),
      );
      shape = 6;
      await controller.refresh();
      await draw();
      expect(
        find.byType(RawImage),
        findsNothing,
        reason: 'missing new shape cannot display stale sprite',
      );
      shape = 5;
      visible = false;
      await controller.refresh();
      await draw();
      expect(find.byType(RawImage), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}
