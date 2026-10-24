import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  Map<Object?, Object?> display({
    required String name,
    required List<int> rect,
    int rotation = 0,
    bool primary = false,
  }) {
    return <Object?, Object?>{
      'name': name,
      'left': rect[0],
      'top': rect[1],
      'right': rect[2],
      'bottom': rect[3],
      'rotation': rotation,
      'primary': primary,
    };
  }

  test('parses the announced display list and finds the streamed output', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'width': 2560,
      'height': 1440,
      'originX': 0,
      'originY': 0,
      'displays': <Object?>[
        display(name: r'\\.\DISPLAY1', rect: <int>[0, 0, 2560, 1440], primary: true),
        display(
          name: r'\\.\DISPLAY2',
          rect: <int>[-1080, 0, 0, 1920],
          rotation: 90,
        ),
      ],
    });

    expect(status.displays.length, 2);
    expect(status.displays[0].index, 0);
    expect(status.displays[0].label, 'DISPLAY1');
    expect(status.displays[0].primary, isTrue);
    expect(status.displays[0].width, 2560);
    expect(status.displays[1].index, 1);
    expect(status.displays[1].rotation, 90);
    expect(status.displays[1].width, 1080);
    expect(status.streamedDisplayIndex, 0);
  });

  test('identifies a stream on a negative-origin secondary display', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'width': 1080,
      'height': 1920,
      'originX': -1080,
      'originY': 0,
      'displays': <Object?>[
        display(name: r'\\.\DISPLAY1', rect: <int>[0, 0, 2560, 1440], primary: true),
        display(
          name: r'\\.\DISPLAY2',
          rect: <int>[-1080, 0, 0, 1920],
          rotation: 90,
        ),
      ],
    });

    expect(status.streamedDisplayIndex, 1);
  });

  test('follows a mid-session stream update to another output', () {
    final ViewerStatus before = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'width': 2560,
      'height': 1440,
      'originX': 0,
      'originY': 0,
      'displays': <Object?>[
        display(name: r'\\.\DISPLAY1', rect: <int>[0, 0, 2560, 1440], primary: true),
        display(name: r'\\.\DISPLAY2', rect: <int>[2560, 0, 4480, 1080]),
      ],
    });
    expect(before.streamedDisplayIndex, 0);

    // The server rebuilt its capture and re-announced: the new picture size
    // and origin describe DISPLAY2. Nothing else in the status changes shape.
    final ViewerStatus after = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'width': 1920,
      'height': 1080,
      'originX': 2560,
      'originY': 0,
      'displays': <Object?>[
        display(name: r'\\.\DISPLAY1', rect: <int>[0, 0, 2560, 1440], primary: true),
        display(name: r'\\.\DISPLAY2', rect: <int>[2560, 0, 4480, 1080]),
      ],
    });
    expect(after.streamedDisplayIndex, 1);
    expect(after.width, 1920);
    expect(after.originX, 2560);
  });

  test('keeps the source size for pointer space when the picture is scaled', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'width': 1280,
      'height': 720,
      'sourceWidth': 2560,
      'sourceHeight': 1440,
      'published': 3,
      'originX': 0,
      'originY': 0,
      'displays': <Object?>[
        display(name: r'\\.\DISPLAY1', rect: <int>[0, 0, 2560, 1440], primary: true),
      ],
    });

    expect(status.width, 1280);
    expect(status.height, 720);
    expect(status.sourceWidth, 2560);
    expect(status.sourceHeight, 1440);
    expect(status.hasFrame, isTrue);
    // The display rect is the desktop, so it matches the source size, not the
    // smaller picture the host encodes.
    expect(status.streamedDisplayIndex, 0);
  });

  test('falls back to the encoded size when the source size is absent', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'width': 1920,
      'height': 1080,
    });

    expect(status.sourceWidth, 1920);
    expect(status.sourceHeight, 1080);
  });

  test('skips malformed entries and tolerates a missing list', () {
    final ViewerStatus withBadEntry = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'displays': <Object?>[
        display(name: r'\\.\DISPLAY1', rect: <int>[0, 0, 0, 0]),
        display(name: r'\\.\DISPLAY2', rect: <int>[0, 0, 1920, 1080], primary: true),
      ],
    });
    expect(withBadEntry.displays.length, 1);
    expect(withBadEntry.displays.single.index, 1);

    const ViewerStatus withoutList = ViewerStatus();
    expect(withoutList.displays, isEmpty);
    expect(withoutList.streamedDisplayIndex, isNull);
  });

  test('a merged stream has no single streamed output', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'mergedLayout': true,
      'width': 3640,
      'height': 1920,
      'originX': -1080,
      'originY': 0,
      'displays': <Object?>[
        display(name: r'\\.\DISPLAY1', rect: <int>[0, 0, 2560, 1440], primary: true),
        display(name: r'\\.\DISPLAY2', rect: <int>[-1080, 0, 0, 1920], rotation: 90),
      ],
    });

    expect(status.mergedLayout, isTrue);
    expect(status.streamedDisplayIndex, isNull);
    expect(status.displays.length, 2);
    expect(status.originX, -1080);
  });

  test('an absent merged flag keeps the single-display path', () {
    const ViewerStatus status = ViewerStatus();
    expect(status.mergedLayout, isFalse);
  });

  test('separates accepted switches from refusals', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'displaySwitches': 2,
      'displaySwitchFailures': 1,
      'displaySwitchError': 'display 3 is not attached',
    });

    expect(status.displaySwitches, 2);
    expect(status.displaySwitchFailures, 1);
    expect(status.displaySwitchError, 'display 3 is not attached');
    const ViewerStatus fresh = ViewerStatus();
    expect(fresh.displaySwitches, 0);
    expect(fresh.displaySwitchFailures, 0);
    expect(fresh.displaySwitchError, isEmpty);
  });
}
