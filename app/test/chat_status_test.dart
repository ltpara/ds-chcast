import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries chat counters and the last failure reason', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'chatSent': 3,
      'chatReceived': 5,
      'chatFailures': 1,
      'chatError': 'send: connection closed',
    });
    expect(status.chatSent, 3);
    expect(status.chatReceived, 5);
    expect(status.chatFailures, 1);
    expect(status.chatError, contains('closed'));
  });

  test('chat fields default to zero and empty', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.chatSent, 0);
    expect(status.chatReceived, 0);
    expect(status.chatFailures, 0);
    expect(status.chatError, isEmpty);
  });
}
