import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/recording_upload_queue.dart';

void main() {
  late Directory directory;
  late File queueFile;

  setUp(() {
    directory = Directory.systemTemp.createTempSync('chast-upload-queue-');
    queueFile = File('${directory.path}/queue.json');
  });

  tearDown(() {
    if (directory.existsSync()) directory.deleteSync(recursive: true);
  });

  RecordingUploadJob job({
    String id = '0123456789abcdef0123456789abcdef',
    String server = 'https://example.test/',
    String username = 'alice',
    String path = r'C:\Videos\chast-20261122-120000.mp4',
    String uploadId = '',
  }) => RecordingUploadJob(
    id: id,
    path: path,
    server: server,
    username: username,
    hostId: 'host-1',
    peerId: 'host-1',
    startedAtMs: 1000,
    endedAtMs: 5000,
    fileSize: 42,
    fileModifiedAtMs: 5000,
    uploadId: uploadId,
  );

  test(
    'queue survives store recreation without persisting bearer tokens',
    () async {
      final RecordingUploadQueueStore first = RecordingUploadQueueStore(
        file: queueFile,
      );
      await first.enqueue(job());
      await first.updateUploadId(
        '0123456789abcdef0123456789abcdef',
        'abcdef0123456789abcdef0123456789',
      );

      final RecordingUploadQueueStore afterRestart = RecordingUploadQueueStore(
        file: queueFile,
      );
      final List<RecordingUploadJob> restored = await afterRestart.load();
      expect(restored, hasLength(1));
      expect(restored.single.uploadId, 'abcdef0123456789abcdef0123456789');
      expect(restored.single.durationMs, 4000);
      expect(await afterRestart.protectedPaths(), contains(job().path));
      expect(await queueFile.readAsString(), isNot(contains('token')));
    },
  );

  test(
    'same path is deduplicated but account identity remains isolated',
    () async {
      final RecordingUploadQueueStore store = RecordingUploadQueueStore(
        file: queueFile,
      );
      await store.enqueue(job());
      await store.enqueue(job(id: '1123456789abcdef0123456789abcdef'));
      await store.enqueue(
        job(id: '2123456789abcdef0123456789abcdef', username: 'bob'),
      );
      await store.enqueue(
        job(
          id: '3123456789abcdef0123456789abcdef',
          server: 'https://other.test',
        ),
      );

      final List<RecordingUploadJob> jobs = await store.load();
      expect(jobs, hasLength(3));
      expect(
        jobs.where(
          (RecordingUploadJob item) =>
              item.username == 'alice' && item.server == 'https://example.test',
        ),
        hasLength(1),
      );
      final RecordingUploadJob alice = job(
        server: 'https://example.test',
        username: 'alice',
      );
      expect(
        jobs.where(
          (RecordingUploadJob item) =>
              item.belongsTo(server: alice.server, username: alice.username),
        ),
        hasLength(1),
      );
    },
  );

  test('multiple engines serialize the upload worker lease', () async {
    final RecordingUploadQueueStore first = RecordingUploadQueueStore(
      file: queueFile,
    );
    final RecordingUploadQueueStore second = RecordingUploadQueueStore(
      file: queueFile,
    );
    final RecordingUploadWorkerLease held = await first.acquireWorker();
    expect(await second.tryAcquireWorker(), isNull);
    bool acquired = false;
    final Future<RecordingUploadWorkerLease> waiting = second
        .acquireWorker()
        .then((RecordingUploadWorkerLease lease) {
          acquired = true;
          return lease;
        });
    await Future<void>.delayed(const Duration(milliseconds: 50));
    expect(acquired, isFalse);
    await held.release();
    final RecordingUploadWorkerLease next = await waiting;
    expect(acquired, isTrue);
    await next.release();
    final RecordingUploadWorkerLease? available = await second
        .tryAcquireWorker();
    expect(available, isNotNull);
    await available!.release();
  });

  test('malformed queue is not silently replaced by a new job', () async {
    await queueFile.writeAsString(jsonEncode(<String, Object?>{'version': 99}));
    final String before = await queueFile.readAsString();
    final RecordingUploadQueueStore store = RecordingUploadQueueStore(
      file: queueFile,
    );

    await expectLater(store.enqueue(job()), throwsFormatException);
    expect(await queueFile.readAsString(), before);
  });

  test(
    'recording intents survive store recreation and hold a process lease',
    () async {
      final RecordingUploadQueueStore first = RecordingUploadQueueStore(
        file: queueFile,
      );
      const RecordingUploadIntent intent = RecordingUploadIntent(
        id: '4123456789abcdef0123456789abcdef',
        path: r'C:\Videos\reserved.mp4',
        server: 'https://example.test',
        username: 'alice',
        hostId: 'host-1',
        peerId: 'host-1',
        startedAtMs: 1000,
      );
      final RecordingUploadWorkerLease lease = await first.beginRecording(
        intent,
      );
      final RecordingUploadQueueStore afterRestart = RecordingUploadQueueStore(
        file: queueFile,
      );

      final List<RecordingUploadIntent> restored = await afterRestart
          .loadIntents();
      expect(restored, hasLength(1));
      expect(restored.single.id, intent.id);
      expect(restored.single.path, intent.path);
      expect(restored.single.server, intent.server);
      expect(await afterRestart.tryAcquireIntentLease(intent.id), isNull);
      await lease.release();
      final RecordingUploadWorkerLease? recovered = await afterRestart
          .tryAcquireIntentLease(intent.id);
      expect(recovered, isNotNull);
      await recovered!.release();
    },
  );

  test('recovery enqueues finalized rollover segments once', () async {
    final File base = File('${directory.path}/capture.mp4')
      ..writeAsBytesSync(<int>[
        ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
        ..._box('mdat', <int>[1, 2, 3]),
        ..._box('moov', <int>[4, 5]),
      ]);
    final File segment = File('${directory.path}/capture-2.mp4')
      ..writeAsBytesSync(<int>[
        ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
        ..._box('mdat', <int>[6, 7, 8]),
        ..._box('moov', <int>[9, 10]),
      ]);
    final RecordingUploadQueueStore store = RecordingUploadQueueStore(
      file: queueFile,
    );
    final RecordingUploadWorkerLease lease = await store.beginRecording(
      RecordingUploadIntent(
        id: '5123456789abcdef0123456789abcdef',
        path: base.absolute.path,
        server: 'https://example.test',
        username: 'alice',
        hostId: 'host-1',
        peerId: 'host-1',
        startedAtMs: DateTime.now()
            .subtract(const Duration(minutes: 1))
            .millisecondsSinceEpoch,
      ),
    );
    await lease.release();

    await store.recoverIntents();
    await store.recoverIntents();

    String normalizePath(String path) =>
        path.replaceAll('/', '\\').toLowerCase();
    final List<String> queuedPaths = (await store.load())
        .map((RecordingUploadJob job) => normalizePath(job.path))
        .toList();
    expect(queuedPaths, contains(normalizePath(base.absolute.path)));
    expect(queuedPaths, contains(normalizePath(segment.absolute.path)));
    expect(queuedPaths, hasLength(2));
    expect(await store.loadIntents(), isEmpty);
  });

  test(
    'recovery retains an intent until every rollover segment is finalized',
    () async {
      final File base = File('${directory.path}/partial.mp4')
        ..writeAsBytesSync(<int>[
          ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
          ..._box('mdat', <int>[1, 2, 3]),
          ..._box('moov', <int>[4, 5]),
        ]);
      final File segment = File('${directory.path}/partial-2.mp4')
        ..writeAsBytesSync(<int>[
          ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
          ..._box('mdat', <int>[6, 7, 8]),
        ]);
      final RecordingUploadQueueStore store = RecordingUploadQueueStore(
        file: queueFile,
      );
      final RecordingUploadWorkerLease lease = await store.beginRecording(
        RecordingUploadIntent(
          id: '6123456789abcdef0123456789abcdef',
          path: base.absolute.path,
          server: 'https://example.test',
          username: 'alice',
          hostId: 'host-1',
          peerId: 'host-1',
          startedAtMs: DateTime.now()
              .subtract(const Duration(minutes: 1))
              .millisecondsSinceEpoch,
        ),
      );
      await lease.release();

      await store.recoverIntents();
      expect(await store.tryCountUnfinalizedMp4Segments(), 1);
      expect(await store.load(), hasLength(1));
      expect(await store.loadIntents(), hasLength(1));
      expect(segment.existsSync(), isTrue);

      segment.writeAsBytesSync(<int>[
        ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
        ..._box('mdat', <int>[6, 7, 8]),
        ..._box('moov', <int>[9, 10]),
      ]);
      await store.recoverIntents();
      expect(await store.tryCountUnfinalizedMp4Segments(), 0);
      expect(await store.load(), hasLength(2));
      expect(await store.loadIntents(), isEmpty);
    },
  );

  test('only structurally finalized MP4 files are recoverable', () async {
    final File complete = File('${directory.path}/complete.mp4')
      ..writeAsBytesSync(<int>[
        ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
        ..._box('mdat', <int>[1, 2, 3]),
        ..._box('moov', <int>[4, 5]),
      ]);
    final File incomplete = File('${directory.path}/incomplete.mp4')
      ..writeAsBytesSync(<int>[
        ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
        ..._box('mdat', <int>[1, 2, 3]),
      ]);

    expect(await isFinalizedMp4(complete), isTrue);
    expect(await isFinalizedMp4(incomplete), isFalse);
    expect(
      await isFinalizedMp4(File('${directory.path}/missing.mp4')),
      isFalse,
    );
  });
}

List<int> _box(String type, List<int> payload) {
  final ByteData size = ByteData(4)..setUint32(0, payload.length + 8);
  return <int>[...size.buffer.asUint8List(), ...type.codeUnits, ...payload];
}
