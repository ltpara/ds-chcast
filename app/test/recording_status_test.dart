import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the recording counters and the newest file', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'recording': true,
      'recordPath': r'C:\Users\me\Videos\chast-20261027-081500.mp4',
      'recordError': '',
      'recordSamples': 541,
      'recordKeyframes': 3,
      'recordSkipped': 0,
      'recordSegments': 2,
    });
    expect(status.recording, isTrue);
    expect(status.recordPath, endsWith('chast-20261027-081500.mp4'));
    expect(status.recordError, isEmpty);
    expect(status.recordSamples, 541);
    expect(status.recordKeyframes, 3);
    expect(status.recordSkipped, 0);
    expect(status.recordSegments, 2);
  });

  test('recording defaults to off with no file and no error', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.recording, isFalse);
    expect(status.recordPath, '');
    expect(status.recordError, '');
    expect(status.recordSamples, 0);
    expect(status.recordSegments, 0);
  });

  test('recording needs a live session and the host consent bit', () {
    ViewerStatus status(Map<String, Object?> fields) => ViewerStatus.fromMap(
      <Object?, Object?>{'state': 'streaming', ...fields},
    );
    expect(
      status(<String, Object?>{
        'permissions': ViewerPermission.record,
      }).acceptsRecording,
      isTrue,
    );
    expect(
      status(<String, Object?>{'permissions': 0}).acceptsRecording,
      isFalse,
    );
    expect(
      ViewerStatus.fromMap(<Object?, Object?>{
        'state': 'ended',
        'permissions': ViewerPermission.record,
      }).acceptsRecording,
      isFalse,
    );
    // Native revocation finalizes the recorder; the UI must not offer another
    // start. The native loopback test verifies the actual recorder lifecycle.
    final ViewerStatus revoked = status(<String, Object?>{
      'permissions': 0,
      'recording': false,
    });
    expect(revoked.recording, isFalse);
    expect(revoked.acceptsRecording, isFalse);
  });

  test('permission revocation is separate from recording failures', () {
    ViewerStatus snapshot(Map<String, Object?> fields) =>
        ViewerStatus.fromMap(<Object?, Object?>{
          'state': 'streaming',
          'permissions': ViewerPermission.record,
          ...fields,
        });
    final ViewerStatus recording = snapshot(<String, Object?>{
      'recording': true,
      'recordError': '',
    });
    final ViewerStatus revoked = snapshot(<String, Object?>{
      'permissions': 0,
      'recording': false,
      'recordError': '',
    });
    expect(recordingPermissionWasRevoked(recording, revoked), isTrue);
    expect(
      recordingPermissionWasRevoked(
        recording,
        snapshot(<String, Object?>{
          'permissions': 0,
          'recording': false,
          'recordError': 'disk full',
        }),
      ),
      isFalse,
    );
    expect(
      recordingPermissionWasRevoked(
        recording,
        snapshot(<String, Object?>{
          'permissions': 0,
          'recording': false,
          'recordError': 'the host revoked recording',
        }),
      ),
      isTrue,
    );
    expect(
      recordingPermissionWasRevoked(
        snapshot(<String, Object?>{'recording': false}),
        revoked,
      ),
      isFalse,
    );
  });

  test('a failed recording keeps its reason for the toolbar', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'recording': false,
      'recordError': 'MP4 recording supports H.264 only; the host encoded HEVC',
    });
    expect(status.recording, isFalse);
    expect(status.recordError, contains('H.264 only'));
  });
}
