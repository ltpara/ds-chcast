import 'dart:io';

import 'package:chast_viewer/recording_upload_queue.dart';

Future<void> main(List<String> args) async {
  if (args.length < 2) {
    stderr.writeln('usage: <seed|verify|hold|probe> <queue-file> [args]');
    exitCode = 64;
    return;
  }
  final RecordingUploadQueueStore store = RecordingUploadQueueStore(
    file: File(args[1]),
  );
  switch (args[0]) {
    case 'seed':
      await store.enqueue(
        const RecordingUploadJob(
          id: '0123456789abcdef0123456789abcdef',
          path: r'C:\recordings\recovered.mp4',
          server: 'https://example.test',
          username: 'alice',
          hostId: 'host-1',
          peerId: 'host-1',
          startedAtMs: 1000,
          endedAtMs: 5000,
          fileSize: 42,
          fileModifiedAtMs: 5000,
        ),
      );
    case 'verify':
      final List<RecordingUploadJob> jobs = await store.load();
      if (jobs.length != 1 ||
          jobs.single.id != '0123456789abcdef0123456789abcdef' ||
          !jobs.single.belongsTo(
            server: 'https://example.test/',
            username: 'alice',
          ) ||
          (args.length == 3 &&
              args[2] == 'progress' &&
              jobs.single.uploadId != 'abcdef0123456789abcdef0123456789')) {
        stderr.writeln(
          'persisted recording job did not survive process restart',
        );
        exitCode = 1;
      }
    case 'hold':
      if (args.length != 4) {
        stderr.writeln('hold requires ready-file and duration-ms');
        exitCode = 64;
        return;
      }
      final RecordingUploadWorkerLease lease = await store.acquireWorker();
      try {
        await store.updateUploadId(
          '0123456789abcdef0123456789abcdef',
          'abcdef0123456789abcdef0123456789',
        );
        await File(args[2]).writeAsString('ready', flush: true);
        await Future<void>.delayed(Duration(milliseconds: int.parse(args[3])));
      } finally {
        await lease.release();
      }
    case 'probe':
      final RecordingUploadWorkerLease? lease = await store.tryAcquireWorker();
      if (lease == null) {
        exitCode = 10;
        return;
      }
      await lease.release();
    default:
      stderr.writeln('unknown mode: ${args[0]}');
      exitCode = 64;
  }
}
