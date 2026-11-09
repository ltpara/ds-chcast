import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/pages/session_page.dart';
import 'package:chast_viewer/pages/display_window_page.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/trust_store.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/display_stream_view.dart';
import 'package:chast_viewer/widgets/remote_toolbar.dart';
import 'package:chast_viewer/widgets/remote_view.dart';
import 'package:chast_viewer/widgets/keyboard_mode_button.dart';

void main() {
  testWidgets(
    'session selects independent textures, routes input and survives empty selection',
    (tester) async {
      const native = MethodChannel('chast/viewer');
      const gpu = MethodChannel('flutter_gpu_texture_renderer');
      final messenger = tester.binding.defaultBinaryMessenger;
      final dir = Directory.systemTemp.createTempSync('chast-displays-');
      StreamPreferences.configPathOverride = '${dir.path}/preferences.json';
      ViewPreferences.configPathOverride = '${dir.path}/preferences.json';
      final started = Completer<void>();
      var starts = 0;
      var multi = false,
          request = 0,
          results = 0,
          nextTexture = 100,
          nextStream = 7;
      var focused = -1;
      final selected = <int, int>{};
      final captureSets = <List<int>>[];
      final events = <String>[];
      final inputs = <Map<Object?, Object?>>[];
      final windowStreams = <int>[];
      final keyboardModes = <String>[];
      final descriptors = [
        {
          'name': 'left',
          'left': -1280,
          'top': -100,
          'right': 0,
          'bottom': 860,
          'primary': false,
        },
        {
          'name': 'right',
          'left': 0,
          'top': 0,
          'right': 1920,
          'bottom': 1080,
          'primary': true,
        },
      ];
      messenger.setMockMethodCallHandler(gpu, (call) async {
        if (call.method == 'registerTexture') return nextTexture++;
        final id = (call.arguments as Map)['id'] as int;
        if (call.method == 'output') return id * 100;
        events.add('unregister:$id');
        return 0;
      });
      messenger.setMockMethodCallHandler(native, (call) async {
        switch (call.method) {
          case 'start':
            starts++;
            started.complete();
            return true;
          case 'openSessionWindow':
            fail('A display window must not create a second session');
          case 'openDisplayWindow':
            windowStreams.add((call.arguments as Map)['stream'] as int);
            return true;
          case 'setKeyboardMode':
            keyboardModes.add(call.arguments as String);
            return null;
          case 'loadPassword':
            return '';
          case 'status':
            return {
              'state': 'streaming',
              'width': 640,
              'height': 480,
              'sourceWidth': 1280,
              'sourceHeight': 960,
              'originX': -1280,
              'originY': -100,
              'published': 1,
              'inputEnabled': true,
              'permissions': ViewerPermission.record,
              'displays': descriptors,
            };
          case 'captureDisplays':
            final args = call.arguments as Map;
            request = args['requestId'] as int;
            results++;
            multi = true;
            final wanted = (args['set'] as List).cast<int>();
            captureSets.add(List<int>.from(wanted));
            selected.removeWhere((index, _) => !wanted.contains(index));
            for (final index in wanted) {
              selected.putIfAbsent(index, () => nextStream++);
            }
            if (!selected.containsValue(focused)) {
              focused = selected.isEmpty ? -1 : selected.values.first;
            }
            return true;
          case 'displayStreams':
            return {
              'active': multi,
              'requestId': request,
              'results': results,
              'focused': focused,
              'streams': [
                for (final e in selected.entries)
                  {
                    'streamId': e.value,
                    'displayIndex': e.key,
                    'width': 640,
                    'height': 480,
                    'sourceWidth': e.key == 0 ? 1280 : 1920,
                    'sourceHeight': e.key == 0 ? 960 : 1080,
                    'originX': e.key == 0 ? -1280 : 0,
                    'originY': e.key == 0 ? -100 : 0,
                    'published': 1,
                    'bound': true,
                    'ready': true,
                  },
              ],
            };
          case 'takeDisplayCursors':
            return [];
          case 'bindDisplayOutput':
            final args = call.arguments as Map;
            events.add('bind:${args['streamId']}:${args['output']}');
            return true;
          case 'focusDisplay':
            focused = call.arguments as int;
            return true;
          case 'sendInput':
            inputs.add(call.arguments as Map<Object?, Object?>);
            return true;
          case 'stop':
            events.add('stop');
            return null;
          case 'takeCompletedFiles':
            return [];
          case 'refreshRemoteCursor':
            return false;
          default:
            return null;
        }
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(native, null);
        messenger.setMockMethodCallHandler(gpu, null);
        StreamPreferences.configPathOverride = null;
        ViewPreferences.configPathOverride = null;
        dir.deleteSync(recursive: true);
      });
      await tester.pumpWidget(
        MaterialApp(
          home: SessionPage(
            target: const ConnectTarget('127.0.0.1', 1),
            trustStore: TrustStore(file: File('${dir.path}/trust.json')),
          ),
        ),
      );
      for (var i = 0; i < 100 && !started.isCompleted; i++) {
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 20)),
        );
        await tester.pump();
      }
      expect(started.isCompleted, isTrue);
      Future<void> poll() async {
        await tester.pump(const Duration(milliseconds: 220));
        await tester.pump();
      }

      await poll();
      RemoteToolbar toolbar() =>
          tester.widget<RemoteToolbar>(find.byType(RemoteToolbar));
      toolbar().onIndependentDisplays!();
      await poll();
      await poll();
      expect(find.byType(DisplayStreamView), findsNWidgets(2));
      expect(
        tester
            .widgetList<RemoteView>(find.byType(RemoteView))
            .map((v) => v.textureId),
        [101, 102],
      );

      final int subscriptionRequests = captureSets.length;
      toolbar().onOpenDisplayWindow!(1);
      await poll();
      expect(windowStreams, [selected[1]]);
      expect(starts, 1);
      expect(captureSets.length, subscriptionRequests);
      toolbar().onKeyboardMode!(ViewerKeyboardMode.local);
      await poll();
      expect(toolbar().keyboardMode, ViewerKeyboardMode.local);
      expect(keyboardModes, contains('local'));
      expect(ViewPreferences.load(peerId: '127.0.0.1:1').keyboardMode, 'local');
      expect(ViewPreferences.load().keyboardMode, 'physical');
      toolbar().onKeyboardMode!(ViewerKeyboardMode.physical);
      await poll();
      toolbar().onSelectDisplay(kViewerDisplayAll);
      await tester.pump();
      expect(toolbar().activeDisplay, kViewerDisplayAll);
      expect(find.byType(DisplayStreamView), findsNothing);
      expect(find.byType(RemoteView), findsNWidgets(2));
      expect(captureSets.length, subscriptionRequests);

      toolbar().onIndependentDisplays!();
      await tester.pump();
      expect(toolbar().activeDisplay, -3);
      expect(find.byType(DisplayStreamView), findsNWidgets(2));
      expect(captureSets.length, subscriptionRequests);

      toolbar().onSelectDisplay(kViewerDisplayAuto);
      await poll();
      await poll();
      expect(captureSets.last, [0]);
      expect(find.byType(DisplayStreamView), findsNothing);
      expect(tester.widget<RemoteView>(find.byType(RemoteView)).textureId, 101);

      toolbar().onSelectDisplay(kViewerDisplayAll);
      await poll();
      await poll();
      expect(captureSets.last, [0, 1]);
      expect(toolbar().activeDisplay, kViewerDisplayAll);
      expect(find.byType(DisplayStreamView), findsNothing);
      expect(find.byType(RemoteView), findsNWidgets(2));
      expect(starts, 1);

      toolbar().onIndependentDisplays!();
      await tester.pump();
      expect(find.byType(DisplayStreamView), findsNWidgets(2));
      toolbar().onToggleControl();
      await tester.pump();
      final left = tester
          .widgetList<RemoteView>(find.byType(RemoteView))
          .singleWhere((view) => view.textureId == 101);
      left.onButton!(const Offset(100, 50), ViewerMouse.left, true);
      left.onButton!(const Offset(100, 50), ViewerMouse.left, false);
      await tester.pump();
      expect(inputs.take(2).map((i) => i['action']), [
        ViewerMouse.move,
        ViewerMouse.buttonDown,
      ]);
      expect(inputs.first['x'], -1180);
      expect(inputs.first['y'], -50);

      tester
          .widgetList<DisplayStreamView>(find.byType(DisplayStreamView))
          .singleWhere((view) => view.texture.textureId == 101)
          .onClose!();
      await poll();
      await poll();
      expect(tester.widget<RemoteView>(find.byType(RemoteView)).textureId, 103);
      expect(
        events.indexOf('bind:7:0'),
        lessThan(events.indexOf('unregister:101')),
      );
      tester
          .widget<DisplayStreamView>(find.byType(DisplayStreamView))
          .onClose!();
      await poll();
      await poll();
      expect(find.byType(RemoteView), findsNothing);
      expect(find.byType(RemoteToolbar), findsOneWidget);
      expect(toolbar().screenshotAvailable, isFalse);
      expect(toolbar().recordingAvailable, isFalse);
      toolbar().onSelectDisplay(1);
      await poll();
      await poll();
      expect(tester.widget<RemoteView>(find.byType(RemoteView)).textureId, 104);
      expect(starts, 1);
      // A child changed view/input preferences while the owner was inactive.
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await tester.pump();
      ViewPreferences.update(
        peerId: '127.0.0.1:1',
        fit: 1,
        keyboardMode: 'local',
      );
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pump();
      await poll();
      expect(toolbar().fit, ViewFit.original);
      expect(toolbar().keyboardMode, ViewerKeyboardMode.local);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      await tester.pump();
      expect(
        events.indexOf('bind:10:0'),
        lessThan(events.indexOf('unregister:104')),
      );
      expect(
        events.indexOf('stop'),
        lessThan(events.indexOf('unregister:100')),
      );
      expect(tester.takeException(), isNull);
    },
  );
  testWidgets(
    'display child shares a session, maps coordinates and detaches before closing',
    (tester) async {
      final dir = Directory.systemTemp.createTempSync('chast-display-child-');
      ViewPreferences.configPathOverride = '${dir.path}/preferences.json';
      addTearDown(() {
        ViewPreferences.configPathOverride = null;
        dir.deleteSync(recursive: true);
      });
      const native = MethodChannel('chast/viewer');
      const gpu = MethodChannel('flutter_gpu_texture_renderer');
      final messenger = tester.binding.defaultBinaryMessenger;
      final events = <String>[];
      final inputs = <Map<Object?, Object?>>[];
      final modes = <String>[];
      var present = true;
      messenger.setMockMethodCallHandler(gpu, (call) async {
        if (call.method == 'registerTexture') return 88;
        if (call.method == 'output') return 8800;
        events.add('unregister');
        return 0;
      });
      messenger.setMockMethodCallHandler(native, (call) async {
        switch (call.method) {
          case 'start':
          case 'stop':
          case 'captureDisplays':
            fail('Child must not own the connection or subscriptions');
          case 'status':
            return {'state': 'streaming', 'inputEnabled': true};
          case 'displayStreams':
            return {
              'active': true,
              'focused': 7,
              'streams': [
                if (present)
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
            events.add('bind:${(call.arguments as Map)['output']}');
            return true;
          case 'setKeyCapture':
            return false; // Simulate an unavailable native input hook.
          case 'setKeyboardMode':
            modes.add(call.arguments as String);
            return null;
          case 'sendInput':
            inputs.add(call.arguments as Map<Object?, Object?>);
            return true;
          case 'closeSessionWindow':
            events.add('close');
            return null;
          default:
            return null;
        }
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(native, null);
        messenger.setMockMethodCallHandler(gpu, null);
      });
      await tester.pumpWidget(
        const MaterialApp(home: DisplayWindowPage(streamId: 7)),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 220));
      expect(find.byType(RemoteView), findsOneWidget);
      expect(events, ['bind:8800']);
      tester
          .widget<KeyboardModeButton>(find.byType(KeyboardModeButton))
          .onChanged!(ViewerKeyboardMode.local);
      await tester.pump();
      expect(modes, contains('local'));
      await tester.tap(find.byIcon(Icons.mouse_outlined));
      await tester.pump();
      final view = tester.widget<RemoteView>(find.byType(RemoteView));
      expect(view.captureInput, isTrue);
      await tester.sendKeyEvent(LogicalKeyboardKey.keyA);
      await tester.pump();
      expect(inputs, isEmpty); // Local mode cannot fall back to physical text.
      view.onButton!(const Offset(100, 50), ViewerMouse.left, true);
      view.onButton!(const Offset(100, 50), ViewerMouse.left, false);
      await tester.pump();
      expect(inputs.first['x'], -1180);
      expect(inputs.first['y'], -50);
      expect(inputs[1]['action'], ViewerMouse.buttonDown);
      // A removed stream closes this window instead of leaving a frozen, controllable image.
      present = false;
      await tester.pump(const Duration(milliseconds: 220));
      await tester.pump();
      expect(events, ['bind:8800', 'bind:0', 'unregister', 'close']);
      expect(find.byType(RemoteView), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
  );
}
