import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('a status without a mode is a desktop session', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{});
    expect(status.controlOnly, isFalse);
    expect(status.sessionMode, 'desktop');
  });

  test('control-only sessions report the agreed mode', () {
    final ViewerStatus terminal = ViewerStatus.fromMap(<Object?, Object?>{
      'controlOnly': true,
      'sessionMode': 'terminal',
    });
    expect(terminal.controlOnly, isTrue);
    expect(terminal.sessionMode, 'terminal');

    final ViewerStatus camera = ViewerStatus.fromMap(<Object?, Object?>{
      'controlOnly': true,
      'sessionMode': 'camera',
    });
    expect(camera.sessionMode, 'camera');

    final ViewerStatus forward = ViewerStatus.fromMap(<Object?, Object?>{
      'controlOnly': true,
      'sessionMode': 'port_forward',
    });
    expect(forward.sessionMode, 'port_forward');
  });

  test('an empty mode falls back to desktop', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'sessionMode': '',
    });
    expect(status.sessionMode, 'desktop');
    expect(status.controlOnly, isFalse);
  });
}
