import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/pages/display_window_page.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/remote_view.dart';
import 'package:chast_viewer/widgets/keyboard_mode_button.dart';

void main() {
  testWidgets('display child applies saved peer view and mouse preferences', (
    tester,
  ) async {
    final dir = Directory.systemTemp.createTempSync('chast-review-prefs-');
    ViewPreferences.configPathOverride = '${dir.path}/ui.json';
    ViewPreferences.save(
      const ViewPreferences(
        fit: 1,
        reverseWheel: true,
        swapButtons: true,
        showMyCursor: true,
      ),
      peerId: 'review-peer',
    );
    final inputs = <Map<Object?, Object?>>[];
    final cursors = <Map<Object?, Object?>>[];
    final messenger = tester.binding.defaultBinaryMessenger;
    const native = MethodChannel('chast/viewer');
    const gpu = MethodChannel('flutter_gpu_texture_renderer');
    messenger.setMockMethodCallHandler(gpu, (call) async {
      if (call.method == 'registerTexture') return 88;
      if (call.method == 'output') return 8800;
      return 0;
    });
    messenger.setMockMethodCallHandler(native, (call) async {
      switch (call.method) {
        case 'status':
          return {'state': 'streaming', 'inputEnabled': true};
        case 'displayStreams':
          return {
            'active': true,
            'focused': 7,
            'streams': [
              {
                'streamId': 7,
                'displayIndex': 0,
                'width': 640,
                'height': 480,
                'sourceWidth': 1280,
                'sourceHeight': 960,
                'originX': -1280,
                'originY': -100,
                'published': 2,
                'bound': true,
                'ready': true,
              },
            ],
          };
        case 'takeDisplayCursors':
          return [];
        case 'bindDisplayOutput':
          return true;
        case 'setKeyCapture':
          return false;
        case 'sendInput':
          inputs.add(call.arguments as Map<Object?, Object?>);
          return true;
        case 'showCursor':
          cursors.add(call.arguments as Map<Object?, Object?>);
          return true;
        case 'start':
        case 'stop':
        case 'captureDisplays':
          fail('Display window must not take ownership of the session');
        default:
          return null;
      }
    });
    addTearDown(() {
      messenger.setMockMethodCallHandler(native, null);
      messenger.setMockMethodCallHandler(gpu, null);
      ViewPreferences.configPathOverride = null;
      dir.deleteSync(recursive: true);
    });
    await tester.pumpWidget(
      const MaterialApp(
        home: DisplayWindowPage(streamId: 7, peerId: 'review-peer'),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 220));
    await tester.tap(find.byIcon(Icons.mouse_outlined));
    await tester.pump();
    final view = tester.widget<RemoteView>(find.byType(RemoteView));
    final actualFit = view.fit;
    view.onButton!(const Offset(100, 50), ViewerMouse.left, true);
    view.onButton!(const Offset(100, 50), ViewerMouse.left, false);
    view.onScroll!(
      const Offset(100, 50),
      const Offset(0, kViewerScrollPixelsPerNotch),
    );
    await tester.pump();
    final down = inputs.firstWhere(
      (v) => v['action'] == ViewerMouse.buttonDown,
    );
    final wheel = inputs.firstWhere((v) => v['action'] == ViewerMouse.wheel);
    final actual = [actualFit.name, down['buttons'], wheel['y']];
    expect(actual, ['original', ViewerMouse.right, kViewerWheelDelta]);
    expect(
      inputs.firstWhere((v) => v['action'] == ViewerMouse.buttonUp)['buttons'],
      ViewerMouse.right,
    );

    // The child window forwards the draw-only cursor hint with the same
    // desktop mapping as its mouse input and clears it on exit.
    view.onCursorHint!(const Offset(100, 50));
    await tester.pump();
    expect(cursors, hasLength(1));
    expect(cursors.single, {'x': -1180, 'y': -50, 'visible': true});
    view.onCursorHintExit!();
    await tester.pump();
    expect(cursors.last, {'x': 0, 'y': 0, 'visible': false});

    // Another window changes unrelated settings after this one loaded them.
    ViewPreferences.update(
      peerId: 'review-peer',
      terminalFontSize: 19,
      stats: true,
    );
    await tester.tap(find.byIcon(Icons.fit_screen));
    await tester.pump();
    expect(ViewPreferences.load(peerId: 'review-peer').fit, 0);
    tester
        .widget<KeyboardModeButton>(find.byType(KeyboardModeButton))
        .onChanged!(ViewerKeyboardMode.local);
    await tester.pump();
    final saved = ViewPreferences.load(peerId: 'review-peer');
    expect(saved.keyboardMode, 'local');
    expect(saved.terminalFontSize, 19);
    expect(saved.stats, isTrue);
    expect(saved.swapButtons, isTrue);
    expect(saved.reverseWheel, isTrue);

    // Return from a different window: reload persisted input/view state.
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    await tester.pump();
    ViewPreferences.update(
      peerId: 'review-peer',
      fit: 1,
      reverseWheel: false,
      swapButtons: false,
      keyboardMode: 'physical',
      showMyCursor: false,
    );
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump();
    final refreshed = tester.widget<RemoteView>(find.byType(RemoteView));
    expect(refreshed.fit, ViewFit.original);
    expect(
      tester.widget<KeyboardModeButton>(find.byType(KeyboardModeButton)).mode,
      ViewerKeyboardMode.physical,
    );
    // A reload that turns the preference off must stop new hints.
    final hints = cursors.length;
    refreshed.onCursorHint!(const Offset(10, 10));
    await tester.pump();
    expect(cursors, hasLength(hints));
    inputs.clear();
    refreshed.onButton!(const Offset(100, 50), ViewerMouse.left, true);
    refreshed.onButton!(const Offset(100, 50), ViewerMouse.left, false);
    refreshed.onScroll!(
      const Offset(100, 50),
      const Offset(kViewerScrollPixelsPerNotch, kViewerScrollPixelsPerNotch),
    );
    await tester.pump();
    expect(
      inputs.firstWhere(
        (v) => v['action'] == ViewerMouse.buttonDown,
      )['buttons'],
      ViewerMouse.left,
    );
    expect(
      inputs.firstWhere((v) => v['action'] == ViewerMouse.wheel)['y'],
      -kViewerWheelDelta,
    );
    expect(
      inputs.firstWhere((v) => v['action'] == ViewerMouse.wheelHorizontal)['x'],
      kViewerWheelDelta,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    await tester.pump();
    // Reopening the child uses the saved state instead of a fixed default.
    await tester.pumpWidget(
      const MaterialApp(
        home: DisplayWindowPage(streamId: 7, peerId: 'review-peer'),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 220));
    expect(
      tester.widget<RemoteView>(find.byType(RemoteView)).fit,
      ViewFit.original,
    );
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });
}
