import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the clipboard gate and counters', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'clipboardEnabled': true,
      'clipboardSent': 2,
      'clipboardReceived': 3,
      'clipboardFailed': 1,
    });
    expect(status.clipboardEnabled, isTrue);
    expect(status.clipboardSent, 2);
    expect(status.clipboardReceived, 3);
    expect(status.clipboardFailed, 1);
    expect(status.acceptsClipboard, isTrue);
  });

  test('clipboard defaults to disabled when the host did not announce it', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.clipboardEnabled, isFalse);
    expect(status.clipboardSent, 0);
    expect(status.clipboardReceived, 0);
    expect(status.clipboardFailed, 0);
    expect(status.acceptsClipboard, isFalse);
  });

  test('an enabled clipboard still waits for the stream', () {
    final ViewerStatus connecting = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'handshaking',
      'clipboardEnabled': true,
    });
    expect(connecting.acceptsClipboard, isFalse);
  });

  test('status carries the upload phase, progress and counters', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'permissions': ViewerPermission.fileTransfer,
      'filePhase': ViewerFilePhase.sending,
      'fileName': 'report.pdf',
      'fileRelativePath': 'reports/2026',
      'fileQueue': 3,
      'fileSize': 1000,
      'fileSent': 250,
      'fileError': '',
      'lastFileName': 'previous.bin',
      'lastFileError': 'the host refused it',
      'filesSent': 1,
      'fileFailures': 2,
    });
    expect(status.fileActive, isTrue);
    expect(status.fileProgress, 0.25);
    expect(status.fileName, 'report.pdf');
    expect(status.fileRelativePath, 'reports/2026');
    expect(status.fileQueue, 3);
    expect(status.lastFileName, 'previous.bin');
    expect(status.lastFileError, 'the host refused it');
    expect(status.filesSent, 1);
    expect(status.fileFailures, 2);
    expect(status.acceptsFiles, isTrue);
  });

  test('an upload without a queue reports the root directory', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'filePhase': ViewerFilePhase.hashing,
      'fileName': 'one.bin',
    });
    expect(status.fileRelativePath, '');
    expect(status.fileQueue, 0);
  });

  test('an upload phase or a missing gate never offers a transfer', () {
    final ViewerStatus done = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'permissions': ViewerPermission.clipboard,
      'filePhase': ViewerFilePhase.done,
    });
    expect(done.fileActive, isFalse);
    expect(done.acceptsFiles, isFalse);

    final ViewerStatus offline = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'closed',
      'permissions': ViewerPermission.fileTransfer,
      'filePhase': ViewerFilePhase.hashing,
    });
    expect(offline.fileActive, isFalse);
    expect(offline.acceptsFiles, isFalse);
  });
}
