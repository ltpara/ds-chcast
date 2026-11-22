import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/session_page.dart';
import 'package:chast_viewer/recording_upload_queue.dart';
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
    addTearDown(() async => h.dispose());
    await h.open();
    await h.tick();
    final renewal = Completer<AccountSession?>();
    h.pendingRenewal = renewal;
    h.resolveExpired = () => renewal.future;
    h.account.expired = true;
    h.recording = false;
    await h.waitFor(() => h.renewals == 1);
    expect(h.renewals, 1);
    expect(h.uploads, isEmpty);
    renewal.complete(h.freshAccount);
    await h.waitFor(() => h.uploads.isNotEmpty);
    await h.waitFor(() => h.uploadQueue.removeCount == 1);
    await h.tick();
    await h.waitForWorkerIdle();
    h.pendingRenewal = null;
    expect(h.uploads, ['fresh-token']);
    await h.tick();
    expect(h.uploads, hasLength(1));
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('temporary renewal failure retains the recording and retries', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(() async => h.dispose());
    await h.open();
    await h.tick();
    h.resolveExpired = () async => null;
    h.account.expired = true;
    h.recording = false;
    await h.waitFor(() => h.renewals == 1);
    expect(h.renewals, 1);
    expect(h.uploads, isEmpty);
    expect(
      h.file.existsSync(),
      isTrue,
      reason: 'retention must protect queued uploads',
    );
    await h.waitFor(() => tester.any(find.textContaining('账号登录已过期')));
    await h.tick();
    await h.waitForWorkerIdle();
    h.resolveExpired = () async => h.freshAccount;
    await tester.pump(const Duration(seconds: 10));
    await h.waitFor(() => h.renewals == 2);
    await h.waitFor(() => h.uploads.isNotEmpty);
    await h.waitFor(() => h.uploadQueue.removeCount == 1);
    await h.tick();
    await h.waitForWorkerIdle();
    expect(h.renewals, greaterThanOrEqualTo(2));
    expect(h.uploads, ['fresh-token']);
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('network retry keeps the original recording end time', (
    tester,
  ) async {
    final h = _RecordingHarness(tester)..failFirstUpload = true;
    addTearDown(() async => h.dispose());
    await h.open();
    await h.tick();
    h.recording = false;
    await h.waitFor(() => h.ends.isNotEmpty);
    await h.waitFor(() => tester.any(find.textContaining('录制上传失败')));
    await h.tick();
    await h.waitForWorkerIdle();
    expect(h.ends, hasLength(1));
    await tester.pump(const Duration(seconds: 10));
    await h.waitFor(() => h.ends.length == 2);
    await h.waitFor(() => h.uploadQueue.removeCount == 1);
    await h.tick();
    await h.waitForWorkerIdle();
    expect(h.ends, hasLength(2));
    expect(h.ends[0], h.ends[1]);
    expect(h.uploads, ['old-token']);
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a persisted recording resumes when a session opens', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(() async => h.dispose());
    h.recording = false;
    await h.open(seedPendingUpload: true, expectRecording: false);
    await h.waitFor(() => h.ends.isNotEmpty);
    await h.waitFor(() => h.uploadQueue.removeCount == 1);
    await h.tick();
    await h.waitForWorkerIdle();

    expect(h.ends, hasLength(1));
    expect(h.uploads, ['old-token']);
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a finalized recording intent is recovered after restart', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(() async => h.dispose());
    h.recording = false;
    await h.open(seedInterruptedRecording: true, expectRecording: false);
    await h.waitFor(() => h.uploads.isNotEmpty);
    await h.waitFor(() => h.uploadQueue.removeCount == 1);
    await h.tick();
    await h.waitForWorkerIdle();

    expect(h.uploads, ['old-token']);
    final List<RecordingUploadIntent> restoredIntents =
        await tester.runAsync(() => h.uploadQueue.loadIntents()) ??
        <RecordingUploadIntent>[];
    expect(
      restoredIntents.map((RecordingUploadIntent item) => item.id),
      isNot(contains('5123456789abcdef0123456789abcdef')),
    );
    await tester.pumpWidget(const SizedBox());
    await h.waitForRecordingIntentsIdle();
    expect(await tester.runAsync(() => h.uploadQueue.loadIntents()), isEmpty);
  });

  testWidgets('session claims a display recording intent by file path', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(() async => h.dispose());
    final String id = '6123456789abcdef0123456789abcdef';
    h.uploadQueue.pathIntent = RecordingUploadIntent(
      id: id,
      path: h.file.absolute.path,
      server: h.account.server,
      username: h.account.username,
      hostId: 'host-7',
      peerId: 'host-7',
      startedAtMs: DateTime.now()
          .subtract(const Duration(minutes: 1))
          .millisecondsSinceEpoch,
    );
    await h.open();
    await h.tick();
    h.recording = false;
    await h.waitFor(() => h.uploads.isNotEmpty);
    await h.waitFor(() => h.uploadQueue.removeCount == 1);
    await h.tick();
    await h.waitForWorkerIdle();

    expect(h.uploads, ['old-token']);
    expect(await tester.runAsync(() => h.uploadQueue.loadIntents()), isEmpty);
  });

  testWidgets('an incomplete recording stays local and recoverable', (
    tester,
  ) async {
    final h = _RecordingHarness(tester)..recording = false;
    addTearDown(() async => h.dispose());
    await h.open(
      seedInterruptedRecording: true,
      finalized: false,
      expectRecording: false,
    );
    await h.waitForWorkerIdle();
    await h.waitForRecordingIntentsIdle();
    expect(h.uploads, isEmpty);
    expect(
      await tester.runAsync(() => h.uploadQueue.loadIntents()),
      hasLength(1),
    );
    expect(
      h.dir.listSync().whereType<File>().any(
        (File file) => file.path.endsWith('interrupted.mp4'),
      ),
      isTrue,
    );
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('closing the session cancels a pending upload retry', (
    tester,
  ) async {
    final h = _RecordingHarness(tester);
    addTearDown(() async => h.dispose());
    await h.open();
    await h.tick();
    h.resolveExpired = () async => null;
    h.account.expired = true;
    h.recording = false;
    await h.waitFor(() => h.renewals == 1);
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
  late final _ObservedRecordingUploadQueue uploadQueue =
      _ObservedRecordingUploadQueue(File('${dir.path}/recording-uploads.json'));
  Future<AccountSession?> Function()? resolveExpired;
  Completer<AccountSession?>? pendingRenewal;
  bool recording = true;
  bool started = false;
  bool failFirstUpload = false;
  int renewals = 0;
  final uploads = <String>[];
  final ends = <DateTime>[];
  static const native = MethodChannel('chast/viewer');
  static const gpu = MethodChannel('flutter_gpu_texture_renderer');

  Future<void> open({
    bool seedPendingUpload = false,
    bool seedInterruptedRecording = false,
    bool finalized = true,
    bool expectRecording = true,
  }) async {
    file.writeAsBytesSync(_testMp4Bytes());
    final File olderFile = File('${dir.path}/chast-20261116-110000.mp4')
      ..writeAsBytesSync(_testMp4Bytes());
    if (seedPendingUpload) {
      final FileStat stat = olderFile.statSync();
      await tester.runAsync(() async {
        await uploadQueue.enqueue(
          RecordingUploadJob(
            id: '0123456789abcdef0123456789abcdef',
            path: olderFile.absolute.path,
            server: account.server,
            username: account.username,
            hostId: '',
            peerId: '127.0.0.1:1',
            startedAtMs: 1000,
            endedAtMs: 5000,
            fileSize: stat.size,
            fileModifiedAtMs: stat.modified.millisecondsSinceEpoch,
          ),
        );
        return true;
      });
    }
    if (seedInterruptedRecording) {
      final File interrupted = File('${dir.path}/interrupted.mp4')
        ..writeAsBytesSync(<int>[
          ..._mp4Box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
          ..._mp4Box('mdat', <int>[1, 2, 3]),
          if (finalized) ..._mp4Box('moov', <int>[4, 5]),
        ]);
      final RecordingUploadWorkerLease? lease = await tester.runAsync(
        () async => uploadQueue.beginRecording(
          RecordingUploadIntent(
            id: '5123456789abcdef0123456789abcdef',
            path: interrupted.absolute.path,
            server: account.server,
            username: account.username,
            hostId: '127.0.0.1:1',
            peerId: '127.0.0.1:1',
            startedAtMs: DateTime.now()
                .subtract(const Duration(minutes: 1))
                .millisecondsSinceEpoch,
          ),
        ),
      );
      await tester.runAsync(() => lease!.release());
    }
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
        case 'stop':
          recording = false;
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
          recordingUploadQueue: uploadQueue,
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
      expectRecording,
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

  Future<void> waitFor(bool Function() condition) async {
    for (var i = 0; i < 60 && !condition(); i++) {
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 50)),
      );
      await tester.pump(const Duration(milliseconds: 50));
    }
    expect(condition(), isTrue, reason: 'asynchronous upload did not settle');
  }

  Future<void> dispose() async {
    await tester.pumpWidget(const SizedBox());
    final Completer<AccountSession?>? renewal = pendingRenewal;
    if (renewal != null && !renewal.isCompleted) renewal.complete(null);
    await waitForWorkerIdle();
    await waitForRecordingIntentsIdle();
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      native,
      null,
    );
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(gpu, null);
    StreamPreferences.configPathOverride = null;
    ViewPreferences.configPathOverride = null;
    if (dir.existsSync()) dir.deleteSync(recursive: true);
  }

  Future<void> waitForWorkerIdle() async {
    for (var i = 0; i < 60; i++) {
      final _WorkerProbe? probe = await tester.runAsync(
        () async => _WorkerProbe(await uploadQueue.tryAcquireWorker()),
      );
      final RecordingUploadWorkerLease? lease = probe?.lease;
      if (lease != null) {
        await tester.runAsync(() => lease.release());
        return;
      }
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 50)),
      );
      await tester.pump(const Duration(milliseconds: 50));
    }
    fail('recording upload worker did not release its lock');
  }

  Future<void> waitForRecordingIntentsIdle() async {
    for (int attempt = 0; attempt < 60; attempt++) {
      final List<RecordingUploadIntent> intents =
          await tester.runAsync(() => uploadQueue.loadIntents()) ??
          <RecordingUploadIntent>[];
      bool idle = true;
      for (final RecordingUploadIntent intent in intents) {
        final RecordingUploadWorkerLease? lease = await tester
            .runAsync<RecordingUploadWorkerLease?>(
              () => uploadQueue.tryAcquireIntentLease(intent.id),
            );
        if (lease == null) {
          idle = false;
        } else {
          await tester.runAsync(() => lease.release());
        }
      }
      if (idle) return;
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 50)),
      );
      await tester.pump(const Duration(milliseconds: 50));
    }
    fail('recording intent lease did not release');
  }
}

class _WorkerProbe {
  const _WorkerProbe(this.lease);

  final RecordingUploadWorkerLease? lease;
}

List<int> _mp4Box(String type, List<int> payload) {
  final ByteData size = ByteData(4)..setUint32(0, payload.length + 8);
  return <int>[...size.buffer.asUint8List(), ...type.codeUnits, ...payload];
}

List<int> _testMp4Bytes({bool finalized = true}) => <int>[
  ..._mp4Box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
  ..._mp4Box('mdat', <int>[1, 2, 3]),
  if (finalized) ..._mp4Box('moov', <int>[4, 5]),
];

class _ObservedRecordingUploadQueue extends RecordingUploadQueueStore {
  _ObservedRecordingUploadQueue(File file) : super(file: file);

  int removeCount = 0;
  RecordingUploadIntent? pathIntent;

  @override
  Future<RecordingUploadIntent?> findIntentForPath(String path) async {
    final RecordingUploadIntent? intent = pathIntent;
    if (intent == null ||
        intent.path.replaceAll('/', '\\').toLowerCase() !=
            path.replaceAll('/', '\\').toLowerCase()) {
      return null;
    }
    return intent;
  }

  @override
  Future<void> remove(String jobId) async {
    await super.remove(jobId);
    removeCount++;
  }
}
