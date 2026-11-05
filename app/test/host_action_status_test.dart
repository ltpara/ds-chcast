import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the host-action answer and the block state', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'inputEnabled': true,
      'permissions': ViewerPermission.input |
          ViewerPermission.restart |
          ViewerPermission.blockInput,
      'restartSupported': true,
      'blockInputSupported': true,
      'blockInputActive': true,
      'hostActionAction': ViewerHostAction.blockInput.code,
      'hostActionCode': ViewerHostActionCode.busy.code,
      'hostActionError': 'a display switch just happened',
      'hostActionUpdates': 4,
    });
    expect(status.restartSupported, isTrue);
    expect(status.blockInputSupported, isTrue);
    expect(status.blockInputActive, isTrue);
    expect(status.hostActionAction, ViewerHostAction.blockInput.code);
    expect(status.hostActionCode, ViewerHostActionCode.busy.code);
    expect(status.hostActionError, 'a display switch just happened');
    expect(status.hostActionUpdates, 4);
    expect(status.acceptsRestart, isTrue);
    expect(status.acceptsBlockInput, isTrue);
    expect(status.acceptsLockScreen, isTrue);
  });

  test('host actions default to unavailable', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.restartSupported, isFalse);
    expect(status.blockInputSupported, isFalse);
    expect(status.blockInputActive, isFalse);
    expect(status.hostActionAction, 0);
    expect(status.hostActionCode, 0);
    expect(status.hostActionError, isEmpty);
    expect(status.hostActionUpdates, 0);
    expect(status.acceptsRestart, isFalse);
    expect(status.acceptsBlockInput, isFalse);
    expect(status.acceptsLockScreen, isFalse);
  });

  test('restart and block input need their own bits, locking follows input', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'inputEnabled': true,
      'permissions': ViewerPermission.input,
    });
    expect(status.acceptsRestart, isFalse);
    expect(status.acceptsBlockInput, isFalse);
    expect(status.acceptsLockScreen, isTrue);

    final ViewerStatus blocked = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'inputEnabled': false,
      'permissions': ViewerPermission.blockInput,
    });
    expect(blocked.acceptsBlockInput, isTrue);
    expect(blocked.acceptsLockScreen, isFalse);
  });

  test('host actions stay unavailable until the stream is live', () {
    final ViewerStatus connecting = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'handshaking',
      'inputEnabled': true,
      'permissions': ViewerPermission.input |
          ViewerPermission.restart |
          ViewerPermission.blockInput,
      'restartSupported': true,
      'blockInputSupported': true,
    });
    expect(connecting.acceptsRestart, isFalse);
    expect(connecting.acceptsBlockInput, isFalse);
    expect(connecting.acceptsLockScreen, isFalse);
  });
}
