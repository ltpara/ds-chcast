import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/pages/session_page.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/trust_store.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:chast_viewer/widgets/remote_toolbar.dart';

void main() {
  testWidgets(
    'elevation reconnect retains pin when restart and disconnect arrive together',
    (tester) async {
      const native = MethodChannel('chast/viewer');
      const gpu = MethodChannel('flutter_gpu_texture_renderer');
      const target = ConnectTarget('127.0.0.1', 1);
      final pin = List.filled(64, 'A').join();
      final dir = Directory.systemTemp.createTempSync('chast-elevation-');
      final trustFile = File('${dir.path}/trust.json')
        ..writeAsStringSync(jsonEncode({target.trustKey: pin}));
      StreamPreferences.configPathOverride = '${dir.path}/preferences.json';
      ViewPreferences.configPathOverride = '${dir.path}/preferences.json';
      final starts = <Map<Object?, Object?>>[];
      var state = 'streaming', code = 1, updates = 1, request = 0;
      final messenger = tester.binding.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(gpu, (call) async => 1);
      messenger.setMockMethodCallHandler(native, (call) async {
        switch (call.method) {
          case 'start':
            starts.add(call.arguments as Map<Object?, Object?>);
            if (starts.length > 1) {
              state = 'streaming';
              code = 2;
              updates = 1;
              request = 0;
            }
            return null;
          case 'loadPassword':
            return '';
          case 'status':
            return <String, Object>{
              'state': state,
              'width': 640,
              'height': 480,
              'sourceWidth': 640,
              'sourceHeight': 480,
              'published': 1,
              'inputEnabled': true,
              'serverFingerprint': pin,
              'elevationCode': code,
              'elevationRequestId': request,
              'elevationUpdates': updates,
            };
          case 'requestElevation':
            code = 3;
            request = 42;
            updates++;
            return true;
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
            target: target,
            trustStore: TrustStore(file: trustFile),
          ),
        ),
      );
      Future<void> settleStart(int count) async {
        for (var i = 0; i < 100 && starts.length < count; i++) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 20)),
          );
          await tester.pump();
        }
        expect(starts.length, count);
        await tester.pump(const Duration(milliseconds: 250));
        await tester.pump();
      }

      await settleStart(1);
      RemoteToolbar toolbar() =>
          tester.widget<RemoteToolbar>(find.byType(RemoteToolbar));
      expect(toolbar().elevationAvailable, isTrue);
      toolbar().onElevation!();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byType(AlertDialog), findsOneWidget);
      await tester.tap(
        find.descendant(
          of: find.byType(AlertDialog),
          matching: find.byType(FilledButton),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(toolbar().elevationPending, isTrue);
      code = 4;
      updates++;
      state = 'ended';
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 900));
      await settleStart(2);
      expect(starts.last['fingerprint'], pin);
      expect(starts.last['allowUnverified'], isFalse);
      expect(toolbar().elevated, isTrue);
      expect(toolbar().elevationAvailable, isFalse);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets('does not poll stale native status before start completes', (
    WidgetTester tester,
  ) async {
    const MethodChannel channel = MethodChannel('chast/viewer');
    final Completer<void> starting = Completer<void>();
    final Completer<void> started = Completer<void>();
    int polls = 0;
    final Directory dir = Directory.systemTemp.createTempSync('chast-poll-');
    StreamPreferences.configPathOverride = '${dir.path}/preferences.json';
    ViewPreferences.configPathOverride = '${dir.path}/preferences.json';
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(channel, (
      MethodCall call,
    ) async {
      switch (call.method) {
        case 'start':
          starting.complete();
          await started.future;
          return true;
        case 'status':
          ++polls;
          return <String, Object>{'state': 'idle'};
        case 'loadPassword':
          return '';
        default:
          return null;
      }
    });
    addTearDown(() {
      tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        channel,
        null,
      );
      StreamPreferences.configPathOverride = null;
      ViewPreferences.configPathOverride = null;
      dir.deleteSync(recursive: true);
    });

    await tester.pumpWidget(
      MaterialApp(
        home: SessionPage(
          target: const ConnectTarget('127.0.0.1', 1),
          trustStore: TrustStore(file: File('${dir.path}/trust.json')),
          fileTransferOnly: true,
        ),
      ),
    );
    // Trust-store reads use real file I/O before entering the native start.
    for (int i = 0; i < 100 && !starting.isCompleted; ++i) {
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 20)),
      );
      await tester.pump();
    }
    expect(starting.isCompleted, isTrue);
    await tester.pump(const Duration(seconds: 2));
    expect(polls, 0);

    started.complete();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));
    expect(polls, greaterThan(0));

    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    final int closedPolls = polls;
    await tester.pump(const Duration(seconds: 1));
    expect(polls, closedPolls);
  });
}
