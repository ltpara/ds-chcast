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
}
