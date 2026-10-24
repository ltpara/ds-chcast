import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the privacy answer and the captured desktop', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'permissions': ViewerPermission.privacyMode,
      'privacySupported': true,
      'privacyEnabled': true,
      'privacyCode': 0,
      'privacyError': '',
      'privacyUpdates': 3,
      'desktop': 1,
      'desktopUpdates': 2,
    });
    expect(status.privacySupported, isTrue);
    expect(status.privacyEnabled, isTrue);
    expect(status.privacyCode, 0);
    expect(status.privacyError, isEmpty);
    expect(status.privacyUpdates, 3);
    expect(status.desktop, ViewerDesktopKind.secure);
    expect(status.secureDesktop, isTrue);
    expect(status.desktopUpdates, 2);
    expect(status.acceptsPrivacy, isTrue);
  });

  test('privacy defaults to unavailable and the user desktop', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.privacySupported, isFalse);
    expect(status.privacyEnabled, isFalse);
    expect(status.privacyCode, 0);
    expect(status.privacyError, isEmpty);
    expect(status.privacyUpdates, 0);
    expect(status.desktop, ViewerDesktopKind.defaultDesktop);
    expect(status.secureDesktop, isFalse);
    expect(status.desktopUpdates, 0);
    expect(status.acceptsPrivacy, isFalse);
  });

  test('an unknown desktop code falls back to the user desktop', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'desktop': 9,
    });
    expect(status.desktop, ViewerDesktopKind.defaultDesktop);
    expect(status.secureDesktop, isFalse);
  });

  test('privacy stays unavailable until the stream is live', () {
    final ViewerStatus connecting = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'handshaking',
      'privacySupported': true,
      'permissions': ViewerPermission.privacyMode,
    });
    expect(connecting.acceptsPrivacy, isFalse);
  });

  test('an unreachable desktop also shows the secure-desktop banner', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'desktop': 2,
    });
    expect(status.desktop, ViewerDesktopKind.unavailable);
    expect(status.secureDesktop, isTrue);
  });
}
