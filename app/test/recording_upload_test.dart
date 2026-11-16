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
  testWidgets('a finished recording waits for renewal and uses the new token', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(h.dispose);
    await h.open();
    final renewal = Completer<AccountSession?>();
    h.resolveExpired = () => renewal.future;
    h.account.expired = true;
    h.recording = false;
    await h.tick();
    expect(h.renewals, 1);
    expect(h.uploads, isEmpty);
    renewal.complete(h.freshAccount);
    await h.tick();
    expect(h.uploads, ['fresh-token']);
    await h.tick();
    expect(h.uploads, hasLength(1));
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('temporary renewal failure retains the recording and retries', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(h.dispose);
    await h.open();
    h.resolveExpired = () async => null;
    h.account.expired = true;
    h.recording = false;
    await h.tick();
    expect(h.renewals, 1);
    expect(h.uploads, isEmpty);
    expect(
      h.file.existsSync(),
      isTrue,
      reason: 'retention must protect queued uploads',
    );
    expect(find.textContaining('录制上传失败'), findsOneWidget);
    h.resolveExpired = () async => h.freshAccount;
    await tester.pump(const Duration(seconds: 10));
    await h.tick();
    expect(h.renewals, 2);
    expect(h.uploads, ['fresh-token']);
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('network retry keeps the original recording end time', (
    tester,
  ) async {
    final h = _RecordingHarness(tester)..failFirstUpload = true;
    addTearDown(h.dispose);
    await h.open();
    h.recording = false;
    await h.tick();
    expect(h.ends, hasLength(1));
    await tester.pump(const Duration(seconds: 10));
    await h.tick();
    expect(h.ends, hasLength(2));
    expect(h.ends[0], h.ends[1]);
    expect(h.uploads, ['old-token']);
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('closing the session cancels a pending upload retry', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(h.dispose);
    await h.open();
    h.resolveExpired = () async => null;
    h.account.expired = true;
    h.recording = false;
    await h.tick();
    await tester.pumpWidget(const SizedBox());
    await h.tick(); // The final connection audit also resolves the account.
    final attempts = h.renewals;
    await tester.pump(const Duration(seconds: 20));
    expect(h.renewals, attempts);
    expect(h.file.existsSync(), isTrue);
  });
}

class _ExpiringAccount extends AccountSession {
  _ExpiringAccount()
    : super(
        server: 'http://127.0.0.1:1',
        username: 'alice',
        token: 'old-token',
        expiresAt: DateTime.now().add(const Duration(hours: 1)),
      );
  bool expired = false;
  @override
  bool isActiveAt(DateTime now) => !expired;
}

class _RecordingHarness {
  _RecordingHarness(this.tester);
  final WidgetTester tester;
  final dir = Directory.systemTemp.createTempSync('chast-recording-upload-');
  final account = _ExpiringAccount();
  final freshAccount = AccountSession(
    server: 'http://127.0.0.1:1',
    username: 'alice',
    token: 'fresh-token',
    expiresAt: DateTime.now().add(const Duration(hours: 1)),
  );
  late final File file = File('${dir.path}/chast-20261116-100000.mp4');
  Future<AccountSession?> Function()? resolveExpired;
  bool recording = true;
  bool started = false;
  bool failFirstUpload = false;
  int renewals = 0;
  final uploads = <String>[];
  final ends = <DateTime>[];
  static const native = MethodChannel('chast/viewer');
  static const gpu = MethodChannel('flutter_gpu_texture_renderer');

  Future<void> open() async {
    file.writeAsBytesSync([1, 2, 3]);
    File('${dir.path}/chast-20261116-110000.mp4').writeAsBytesSync([4]);
    StreamPreferences.configPathOverride = '${dir.path}/prefs.json';
    ViewPreferences.configPathOverride = '${dir.path}/prefs.json';
    ViewPreferences.update(recordKeep: 1, autoRecord: false);
    const target = ConnectTarget('127.0.0.1', 1);
    final pin = List.filled(64, 'A').join();
    final trust = File('${dir.path}/trust.json')
      ..writeAsStringSync(jsonEncode({target.trustKey: pin}));
    final messenger = tester.binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(gpu, (_) async => 1);
    messenger.setMockMethodCallHandler(native, (call) async {
      switch (call.method) {
        case 'start':
          started = true;
          return null;
        case 'loadPassword':
          return '';
        case 'status':
          return <String, Object>{
            'state': 'streaming',
            'recording': recording,
            'recordPath': file.path,
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
        default:
          return null;
      }
    });
    await tester.pumpWidget(
      MaterialApp(
        home: SessionPage(
          target: target,
          account: account,
          trustStore: TrustStore(file: trust),
          settings: const ServerSettings(uploadRecordings: true),
          accountResolver: () async {
            if (!account.expired) return account;
            renewals++;
            return resolveExpired == null
                ? freshAccount
                : await resolveExpired!();
          },
          recordingUploader: (account, file, start, end) async {
            ends.add(end);
            if (failFirstUpload && ends.length == 1) {
              throw const SocketException('offline');
            }
            uploads.add(account.token);
          },
          connectionReporter:
              ({
                required settings,
                required token,
                required peerId,
                required startedAt,
                required endedAt,
                required detail,
                required note,
                required auditRef,
                required route,
              }) async {},
        ),
      ),
    );
    for (var i = 0; i < 100 && !started; i++) {
      await tick();
    }
    expect(started, isTrue);
    await tick();
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).recording,
      isTrue,
    );
  }

  Future<void> tick() async {
    await tester.pump(const Duration(milliseconds: 250));
    for (var i = 0; i < 8; i++) {
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 10)),
      );
      await tester.pump();
    }
  }

  void dispose() {
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      native,
      null,
    );
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(gpu, null);
    StreamPreferences.configPathOverride = null;
    ViewPreferences.configPathOverride = null;
    dir.deleteSync(recursive: true);
  }
}
