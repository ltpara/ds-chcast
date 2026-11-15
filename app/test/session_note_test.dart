import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/session_page.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/trust_store.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:chast_viewer/widgets/remote_toolbar.dart';

void main() {
  testWidgets('closing asks for a session note when the option is on', (
    WidgetTester tester,
  ) async {
    final _SessionHarness harness = await _SessionHarness.create(
      tester,
      askNote: true,
    );
    addTearDown(harness.dispose);
    await harness.openSession();
    harness.closeSession();
    await harness.pumpCloseDialog();
    expect(find.byType(AlertDialog), findsOneWidget);
    await tester.enterText(find.byType(TextField), 'VPN was flaky');
    await tester.tap(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(FilledButton),
      ),
    );
    await harness.pumpCloseFinished();
    expect(find.byType(SessionPage), findsNothing);
  });

  testWidgets('skipping the note still closes the session', (
    WidgetTester tester,
  ) async {
    final _SessionHarness harness = await _SessionHarness.create(
      tester,
      askNote: true,
    );
    addTearDown(harness.dispose);
    await harness.openSession();
    harness.closeSession();
    await harness.pumpCloseDialog();
    expect(find.byType(AlertDialog), findsOneWidget);
    await tester.tap(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(TextButton),
      ),
    );
    await harness.pumpCloseFinished();
    expect(find.byType(SessionPage), findsNothing);
  });

  testWidgets('closing stays silent when the option is off', (
    WidgetTester tester,
  ) async {
    final _SessionHarness harness = await _SessionHarness.create(
      tester,
      askNote: false,
    );
    addTearDown(harness.dispose);
    await harness.openSession();
    harness.closeSession();
    await harness.pumpCloseDialog();
    expect(find.byType(AlertDialog), findsNothing);
    await harness.pumpCloseFinished();
    expect(find.byType(SessionPage), findsNothing);
  });

  testWidgets('native window waits for the audit response before closing', (
    WidgetTester tester,
  ) async {
    final Completer<void> requestSeen = Completer<void>();
    final Completer<void> releaseResponse = Completer<void>();
    final _SessionHarness harness = await _SessionHarness.create(
      tester,
      askNote: true,
      windowSession: true,
      connectionReporter:
          ({
            required ServerSettings settings,
            required String token,
            required String peerId,
            required DateTime startedAt,
            required DateTime endedAt,
            required String detail,
            required String note,
            required String auditRef,
            required String route,
          }) async {
            requestSeen.complete();
            await releaseResponse.future;
          },
    );
    addTearDown(harness.dispose);
    await harness.openSession();
    await tester.pump(const Duration(seconds: 1));

    final Future<void> closing = harness.requestNativeClose();
    await harness.pumpCloseDialog();
    expect(find.byType(AlertDialog), findsOneWidget);
    await tester.tap(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(FilledButton),
      ),
    );
    await tester.pump();
    await tester.runAsync(
      () => requestSeen.future.timeout(const Duration(seconds: 3)),
    );
    expect(harness.closeWindowCalls, 0);
    releaseResponse.complete();
    await tester.runAsync(() => closing.timeout(const Duration(seconds: 3)));
    expect(harness.closeWindowCalls, 1);
  });

  testWidgets('main window waits for the audit response before popping', (
    WidgetTester tester,
  ) async {
    final Completer<void> requestSeen = Completer<void>();
    final Completer<void> releaseResponse = Completer<void>();
    final _SessionHarness harness = await _SessionHarness.create(
      tester,
      askNote: false,
      connectionReporter:
          ({
            required ServerSettings settings,
            required String token,
            required String peerId,
            required DateTime startedAt,
            required DateTime endedAt,
            required String detail,
            required String note,
            required String auditRef,
            required String route,
          }) async {
            requestSeen.complete();
            await releaseResponse.future;
          },
    );
    addTearDown(harness.dispose);
    await harness.openSession();
    await tester.pump(const Duration(seconds: 1));

    harness.closeSession();
    await tester.runAsync(
      () => requestSeen.future.timeout(const Duration(seconds: 3)),
    );
    expect(find.byType(SessionPage), findsOneWidget);
    releaseResponse.complete();
    await harness.pumpCloseFinished();
    expect(find.byType(SessionPage), findsNothing);
  });

  testWidgets('system back waits for the audit response before popping', (
    WidgetTester tester,
  ) async {
    final Completer<void> requestSeen = Completer<void>();
    final Completer<void> releaseResponse = Completer<void>();
    final _SessionHarness harness = await _SessionHarness.create(
      tester,
      askNote: false,
      connectionReporter:
          ({
            required ServerSettings settings,
            required String token,
            required String peerId,
            required DateTime startedAt,
            required DateTime endedAt,
            required String detail,
            required String note,
            required String auditRef,
            required String route,
          }) async {
            requestSeen.complete();
            await releaseResponse.future;
          },
    );
    addTearDown(harness.dispose);
    await harness.openSession();
    await tester.pump(const Duration(seconds: 1));

    await harness.systemBack();
    await tester.runAsync(
      () => requestSeen.future.timeout(const Duration(seconds: 3)),
    );
    expect(find.byType(SessionPage), findsOneWidget);
    releaseResponse.complete();
    await harness.pumpCloseFinished();
    expect(find.byType(SessionPage), findsNothing);
  });
}

class _SessionHarness {
  _SessionHarness(
    this.tester,
    this.dir,
    this.account,
    this.trustStore,
    this.windowSession,
    this.connectionReporter,
  );

  final WidgetTester tester;
  final Directory dir;
  final AccountSession account;
  final TrustStore trustStore;
  final bool windowSession;
  final SessionConnectionReporter? connectionReporter;
  final List<Map<Object?, Object?>> starts = <Map<Object?, Object?>>[];
  int closeWindowCalls = 0;

  static Future<_SessionHarness> create(
    WidgetTester tester, {
    required bool askNote,
    String serverUrl = 'http://127.0.0.1:1',
    bool windowSession = false,
    SessionConnectionReporter? connectionReporter,
  }) async {
    const MethodChannel native = MethodChannel('chast/viewer');
    const MethodChannel gpu = MethodChannel('flutter_gpu_texture_renderer');
    const ConnectTarget target = ConnectTarget('127.0.0.1', 1);
    final String pin = List.filled(64, 'A').join();
    final Directory dir = Directory.systemTemp.createTempSync(
      'chast-session-note-',
    );
    final File trustFile = File('${dir.path}/trust.json')
      ..writeAsStringSync(jsonEncode(<String, String>{target.trustKey: pin}));
    StreamPreferences.configPathOverride = '${dir.path}/preferences.json';
    ViewPreferences.configPathOverride = '${dir.path}/preferences.json';
    ViewPreferences.update(askSessionNote: askNote);
    final _SessionHarness harness = _SessionHarness(
      tester,
      dir,
      AccountSession(
        server: serverUrl,
        username: 'alice',
        token: 'account-token',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
      ),
      TrustStore(file: trustFile),
      windowSession,
      connectionReporter,
    );
    final messenger = tester.binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(gpu, (MethodCall call) async => 1);
    messenger.setMockMethodCallHandler(native, (MethodCall call) async {
      switch (call.method) {
        case 'start':
          harness.starts.add(call.arguments as Map<Object?, Object?>);
          return null;
        case 'loadPassword':
          return '';
        case 'status':
          return <String, Object>{
            'state': 'streaming',
            'width': 640,
            'height': 480,
            'sourceWidth': 640,
            'sourceHeight': 480,
            'published': 1,
            'inputEnabled': true,
            'serverFingerprint': pin,
          };
        case 'takeCompletedFiles':
          return <Object?>[];
        case 'refreshRemoteCursor':
          return false;
        case 'closeSessionWindow':
          harness.closeWindowCalls++;
          return null;
        default:
          return null;
      }
    });
    addTearDown(() {
      messenger.setMockMethodCallHandler(native, null);
      messenger.setMockMethodCallHandler(gpu, null);
      StreamPreferences.configPathOverride = null;
      ViewPreferences.configPathOverride = null;
    });
    return harness;
  }

  Future<void> openSession() async {
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (BuildContext context) => Scaffold(
            body: TextButton(
              onPressed: () => Navigator.of(context).push(
                MaterialPageRoute<void>(
                  builder: (_) => SessionPage(
                    target: const ConnectTarget('127.0.0.1', 1),
                    account: account,
                    trustStore: trustStore,
                    windowSession: windowSession,
                    connectionReporter: connectionReporter,
                  ),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pump();
    for (int i = 0; i < 200 && starts.isEmpty; i++) {
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 20)),
      );
      await tester.pump();
    }
    expect(starts, isNotEmpty);
    await tester.pump(const Duration(milliseconds: 250));
    await tester.pump();
  }

  void closeSession() {
    tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).onClose();
  }

  Future<void> systemBack() async {
    expect(await tester.binding.handlePopRoute(), isTrue);
    await tester.pump();
  }

  Future<void> requestNativeClose() async {
    final ByteData data = const StandardMethodCodec().encodeMethodCall(
      const MethodCall('windowCloseRequested'),
    );
    final Completer<void> completed = Completer<void>();
    tester.binding.defaultBinaryMessenger.handlePlatformMessage(
      'chast/viewer',
      data,
      (_) => completed.complete(),
    );
    await completed.future;
  }

  /// Shutdown runs before the prompt, so the dialog needs a few frames.
  Future<void> pumpCloseDialog() async {
    for (int i = 0; i < 10; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
  }

  Future<void> pumpCloseFinished() async {
    for (int i = 0; i < 10; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
  }

  void dispose() {
    dir.deleteSync(recursive: true);
  }
}
