import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the pointer counter and pending flag', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'cursorUpdates': 17,
      'cursorPending': true,
    });
    expect(status.cursorUpdates, 17);
    expect(status.cursorPending, isTrue);
  });

  test('a host that sends no pointer leaves both fields at rest', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.cursorUpdates, 0);
    expect(status.cursorPending, isFalse);
  });
}
