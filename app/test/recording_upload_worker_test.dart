import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/recording_upload_queue.dart';
import 'package:chast_viewer/recording_upload_worker.dart';

void main() {
  late Directory directory;
  late RecordingUploadQueueStore queue;

  setUp(() {
    directory = Directory.systemTemp.createTempSync('chast-upload-worker-');
    queue = RecordingUploadQueueStore(
      file: File('${directory.path}/recording-uploads.json'),
    );
  });

  tearDown(() {
    if (directory.existsSync()) directory.deleteSync(recursive: true);
  });

  final AccountSession account = AccountSession(
    server: 'https://archive.example.test',
    username: 'alice',
    token: 'access-token',
    expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
  );

  test(
    'cold worker recovers and uploads a finalized recording intent',
    () async {
      final File recording = File('${directory.path}/finished.mp4')
        ..writeAsBytesSync(_testMp4());
      final RecordingUploadWorkerLease lease = await queue.beginRecording(
        RecordingUploadIntent(
          id: '0123456789abcdef0123456789abcdef',
          path: recording.absolute.path,
          server: account.server,
          username: account.username,
          hostId: '',
          peerId: 'direct-peer',
          startedAtMs: DateTime.now()
              .subtract(const Duration(minutes: 1))
              .millisecondsSinceEpoch,
        ),
      );
      await lease.release();

      final List<String> uploaded = <String>[];
      final RecordingUploadDrainResult result =
          await RecordingUploadWorker(queue: queue).drain(
            accountResolver: () async => account,
            authorize: (AccountSession _, String _) async => true,
            upload:
                (
                  AccountSession current,
                  RecordingUploadJob job,
                  File file,
                ) async {
                  expect(current.username, account.username);
                  expect(file.existsSync(), isTrue);
                  uploaded.add(job.path);
                },
            enabled: () => true,
            shouldStop: () => false,
          );

      expect(result.uploadedCount, 1);
      expect(result.retryRequired, isFalse);
      expect(uploaded, <String>[recording.absolute.path]);
      expect(await queue.load(), isEmpty);
      expect(await queue.loadIntents(), isEmpty);
    },
  );

  test(
    'worker keeps tasks for another account and honors policy denial',
    () async {
      final File allowedFile = File('${directory.path}/allowed.mp4')
        ..writeAsBytesSync(<int>[1, 2, 3]);
      final File deniedFile = File('${directory.path}/denied.mp4')
        ..writeAsBytesSync(<int>[4, 5, 6]);
      final File otherAccountFile = File('${directory.path}/other-account.mp4')
        ..writeAsBytesSync(<int>[7, 8, 9]);
      await queue.enqueue(_job(allowedFile, account));
      await queue.enqueue(
        _job(
          deniedFile,
          account,
          id: '3123456789abcdef0123456789abcdef',
          hostId: 'blocked-host',
        ),
      );
      await queue.enqueue(
        _job(
          otherAccountFile,
          AccountSession(
            server: account.server,
            username: 'bob',
            token: 'other-token',
            expiresAt: account.expiresAt,
          ),
          id: '1123456789abcdef0123456789abcdef',
          hostId: 'blocked-host',
        ),
      );
      final List<String> uploaded = <String>[];

      final RecordingUploadDrainResult result =
          await RecordingUploadWorker(queue: queue).drain(
            accountResolver: () async => account,
            authorize: (AccountSession _, String hostId) async =>
                hostId != 'blocked-host',
            upload: (AccountSession _, RecordingUploadJob job, File _) async {
              uploaded.add(job.path);
            },
            enabled: () => true,
            shouldStop: () => false,
          );

      expect(result.uploadedCount, 1);
      expect(uploaded, <String>[allowedFile.absolute.path]);
      final List<RecordingUploadJob> remaining = await queue.load();
      expect(remaining, hasLength(2));
      expect(
        remaining.map((RecordingUploadJob job) => job.username),
        containsAll(<String>['alice', 'bob']),
      );
      expect(
        remaining.map((RecordingUploadJob job) => job.hostId),
        contains('blocked-host'),
      );
    },
  );
}

RecordingUploadJob _job(
  File file,
  AccountSession account, {
  String id = '2123456789abcdef0123456789abcdef',
  String hostId = '',
}) {
  final FileStat stat = file.statSync();
  return RecordingUploadJob(
    id: id,
    path: file.absolute.path,
    server: account.server,
    username: account.username,
    hostId: hostId,
    peerId: hostId.isEmpty ? 'direct-peer' : hostId,
    startedAtMs: 1000,
    endedAtMs: 5000,
    fileSize: stat.size,
    fileModifiedAtMs: stat.modified.millisecondsSinceEpoch,
  );
}

List<int> _testMp4() => <int>[
  ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
  ..._box('mdat', <int>[1, 2, 3]),
  ..._box('moov', <int>[4, 5]),
];

List<int> _box(String type, List<int> payload) {
  final int size = payload.length + 8;
  return <int>[
    (size >> 24) & 0xff,
    (size >> 16) & 0xff,
    (size >> 8) & 0xff,
    size & 0xff,
    ...type.codeUnits,
    ...payload,
  ];
}
