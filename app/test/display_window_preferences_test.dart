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
  testWidgets('display windows share media controls through session status', (
    tester,
  ) async {
    bool audioMuted = false;
    bool microphoneEnabled = false;
    bool microphoneGranted = false;
    bool voiceEnabled = false;
    bool recording = false;
    bool audioAec = true;
    bool audioNoiseSuppression = true;
    String audioDevice = '';
    String microphoneDevice = '';
    int permissions = ViewerPermission.microphone | ViewerPermission.record;
    var nextTexture = 80;
    final calls = <String, int>{};
    tester.view.physicalSize = const Size(1200, 800);
    tester.view.devicePixelRatio = 1;
    final messenger = tester.binding.defaultBinaryMessenger;
    const native = MethodChannel('chast/viewer');
    const gpu = MethodChannel('flutter_gpu_texture_renderer');
    messenger.setMockMethodCallHandler(gpu, (call) async {
      if (call.method == 'registerTexture') return nextTexture++;
      if (call.method == 'output') return 8800;
      return 0;
    });
    messenger.setMockMethodCallHandler(native, (call) async {
      switch (call.method) {
        case 'status':
          return {
            'state': 'streaming',
            'inputEnabled': true,
            'permissions': permissions,
            'audioEnabled': true,
            'audioMuted': audioMuted,
            'audioDevice': audioDevice,
            'microphoneDevice': microphoneDevice,
            'audioAec': audioAec,
            'audioNoiseSuppression': audioNoiseSuppression,
            'microphoneEnabled': microphoneEnabled,
            'voiceEnabled': voiceEnabled,
            'recording': recording,
          };
        case 'displayStreams':
          return {
            'active': true,
            'focused': 7,
            'streams': [
              for (final id in [7, 8])
                {
                  'streamId': id,
                  'displayIndex': id - 7,
                  'width': 640,
                  'height': 480,
                  'sourceWidth': 640,
                  'sourceHeight': 480,
                  'originX': (id - 7) * 640,
                  'originY': 0,
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
        case 'setAudioMuted':
          audioMuted = call.arguments as bool;
          calls.update(
            'setAudioMuted',
            (value) => value + 1,
            ifAbsent: () => 1,
          );
          return true;
        case 'listAudioOutputs':
          return [
            {'id': 'speaker-2', 'name': 'USB Headset', 'default': true},
          ];
        case 'selectAudioOutput':
          audioDevice = call.arguments as String;
          calls.update(
            'selectAudioOutput',
            (value) => value + 1,
            ifAbsent: () => 1,
          );
          return true;
        case 'listMicrophones':
          return [
            {'id': 'mic-2', 'name': 'USB Microphone'},
          ];
        case 'selectMicrophone':
          microphoneDevice = call.arguments as String;
          calls.update(
            'selectMicrophone',
            (value) => value + 1,
            ifAbsent: () => 1,
          );
          return true;
        case 'setAudioProcessing':
          final args = call.arguments as Map<Object?, Object?>;
          audioAec = args['aec'] as bool;
          audioNoiseSuppression = args['noiseSuppression'] as bool;
          calls.update(
            'setAudioProcessing',
            (value) => value + 1,
            ifAbsent: () => 1,
          );
          return true;
        case 'setMicrophone':
          calls.update(
            'setMicrophone',
            (value) => value + 1,
            ifAbsent: () => 1,
          );
          if (!microphoneGranted) return false;
          microphoneEnabled = call.arguments as bool;
          return true;
        case 'setVoiceCall':
          voiceEnabled = call.arguments as bool;
          calls.update('setVoiceCall', (value) => value + 1, ifAbsent: () => 1);
          return true;
        case 'setRecording':
          recording =
              (call.arguments as Map<Object?, Object?>)['enabled'] as bool;
          calls.update('setRecording', (value) => value + 1, ifAbsent: () => 1);
          return null;
        default:
          return null;
      }
    });
    addTearDown(() {
      messenger.setMockMethodCallHandler(native, null);
      messenger.setMockMethodCallHandler(gpu, null);
      tester.view.resetPhysicalSize();
      tester.view.resetDevicePixelRatio();
    });

    await tester.pumpWidget(
      const MaterialApp(
        home: Row(
          children: [
            Expanded(child: DisplayWindowPage(streamId: 7)),
            Expanded(child: DisplayWindowPage(streamId: 8)),
          ],
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 220));
    await tester.pump();

    Future<void> nextPoll() async {
      await tester.pump(const Duration(milliseconds: 220));
      await tester.pump();
    }

    await tester.longPress(find.byTooltip('静音（长按选择输出设备）').first);
    await tester.pumpAndSettle();
    await tester.tap(find.text('USB Headset'));
    await tester.pump();
    expect(calls['selectAudioOutput'], 1);
    await nextPoll();
    expect(find.byTooltip('静音（长按选择输出设备）'), findsNWidgets(2));

    await tester.tap(find.byTooltip('静音（长按选择输出设备）').first);
    await tester.pump();
    expect(calls['setAudioMuted'], 1);
    await nextPoll();
    expect(find.byTooltip('取消静音（长按选择输出设备）'), findsNWidgets(2));

    await tester.tap(find.byTooltip('开启麦克风（长按选择设备）').first);
    await tester.pump();
    expect(calls['setMicrophone'], 1);
    expect(find.text('主机未授权麦克风，无法开启'), findsWidgets);
    ScaffoldMessenger.of(
      tester.element(find.byType(Scaffold).first),
    ).hideCurrentSnackBar();
    await tester.pump();
    microphoneGranted = true;
    await tester.tap(find.byTooltip('开启麦克风（长按选择设备）').first);
    await tester.pump();
    await nextPoll();
    expect(find.byTooltip('关闭麦克风（长按选择设备）'), findsNWidgets(2));

    await tester.tap(find.byTooltip('收听主机麦克风').first);
    await tester.pump();
    await nextPoll();
    expect(find.byTooltip('结束语音通话'), findsNWidgets(2));

    await tester.tap(find.byTooltip('录制到本地（保存为 MP4）').first);
    await tester.pump();
    await nextPoll();
    expect(calls['setRecording'], 1);
    expect(find.byTooltip('停止录制（保存为本地 MP4）'), findsNWidgets(2));

    await tester.longPress(find.byTooltip('关闭麦克风（长按选择设备）').first);
    await tester.pumpAndSettle();
    await tester.tap(find.text('USB Microphone'));
    await tester.pump();
    expect(calls['selectMicrophone'], 1);
    await nextPoll();
    await tester.longPress(find.byTooltip('关闭麦克风（长按选择设备）').first);
    await tester.pumpAndSettle();
    await tester.tap(find.text('噪声抑制'));
    await tester.pump();
    expect(calls['setAudioProcessing'], 1);
    await nextPoll();

    await tester.tap(find.byTooltip('关闭麦克风（长按选择设备）').first);
    await tester.pump();
    await nextPoll();
    await tester.tap(find.byTooltip('结束语音通话').first);
    await tester.pump();
    await nextPoll();
    await tester.tap(find.byTooltip('停止录制（保存为本地 MP4）').first);
    await tester.pump();
    await nextPoll();

    permissions = 0;
    await nextPoll();
    expect(find.byTooltip('服务端未开放麦克风'), findsNWidgets(2));
    expect(find.byTooltip('主机未开放录制'), findsNWidgets(2));
    final disabledMicrophone = find.descendant(
      of: find.byTooltip('服务端未开放麦克风').first,
      matching: find.byType(InkWell),
    );
    expect(tester.widget<InkWell>(disabledMicrophone.first).onTap, isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    await tester.pump();
    expect(tester.takeException(), isNull);
  });

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
    expect(cursors.single, {
      'x': -1180,
      'y': -50,
      'visible': true,
      'pressed': false,
    });
    view.onCursorHintPress!(const Offset(100, 50));
    await tester.pump();
    expect(cursors.last, {
      'x': -1180,
      'y': -50,
      'visible': true,
      'pressed': true,
    });
    view.onCursorHintExit!();
    await tester.pump();
    expect(cursors.last, {'x': 0, 'y': 0, 'visible': false, 'pressed': false});

    // Another window changes unrelated settings after this one loaded them.
    ViewPreferences.update(
      peerId: 'review-peer',
      terminalFontSize: 19,
      stats: true,
    );
    await tester.tap(find.byTooltip('视图与光标缩放'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('自适应窗口'));
    await tester.tap(find.text('应用'));
    await tester.pumpAndSettle();
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
