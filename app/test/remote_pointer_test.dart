import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/remote_pointer.dart';
import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('contact slots are stable and recycled lowest-first', () {
    final RemoteContactTracker tracker = RemoteContactTracker();
    expect(tracker.slotFor(7), isNull);
    expect(tracker.acquire(7), 0);
    expect(tracker.acquire(7), 0, reason: 'one pointer keeps its slot');
    expect(tracker.acquire(9), 1);
    expect(tracker.contacts, unorderedEquals(<int>[0, 1]));
    expect(tracker.release(7), 0);
    expect(tracker.slotFor(7), isNull);
    expect(
      tracker.acquire(11),
      0,
      reason: 'the freed slot is handed out first',
    );
    tracker.clear();
    expect(tracker.contacts, isEmpty);
  });

  test('a full tracker refuses further contacts', () {
    final RemoteContactTracker tracker = RemoteContactTracker();
    for (int pointer = 0; pointer < kViewerMaxTouchContacts; pointer++) {
      expect(tracker.acquire(pointer), pointer);
    }
    expect(tracker.acquire(999), isNull);
    expect(tracker.release(999), isNull);
    expect(tracker.release(4), 4);
    expect(tracker.acquire(999), 4);
  });

  test('pressure is normalized to the host 0..1024 range', () {
    expect(windowsPointerPressure(0, 0, 1), 0);
    expect(windowsPointerPressure(0.5, 0, 1), 512);
    expect(windowsPointerPressure(1, 0, 1), 1024);
    expect(windowsPointerPressure(0.25, 0, 0.5), 512);
    expect(windowsPointerPressure(-0.5, 0, 1), 0);
    expect(windowsPointerPressure(2, 0, 1), 1024);
    expect(
      windowsPointerPressure(0.5, 0.5, 0.5),
      0,
      reason: 'a zero-width range means unreported',
    );
    expect(windowsPointerPressure(0.5, 0, 0), 0);
    expect(windowsPointerPressure(double.nan, 0, 1), 0);
    expect(windowsPointerPressure(double.infinity, 0, 1), 0);
  });

  test('tilt maps Flutter azimuth to Windows per-axis degrees', () {
    expect(windowsPenTilt(0, 0), (0, 0));
    expect(
        windowsPenTilt(math.pi / 2, 0),
        (
          0,
          -90,
        ),
        reason: 'pointing toward the top of the screen');
    expect(
        windowsPenTilt(math.pi / 2, math.pi / 2),
        (
          90,
          0,
        ),
        reason: 'pointing right');
    expect(
        windowsPenTilt(math.pi / 2, math.pi),
        (
          0,
          90,
        ),
        reason: 'pointing toward the user');
    expect(windowsPenTilt(math.pi / 2, 3 * math.pi / 2), (-90, 0));
    expect(windowsPenTilt(math.pi / 4, 0), (0, -45));
    expect(windowsPenTilt(double.nan, 0), (0, 0));
    expect(windowsPenTilt(0, double.infinity), (0, 0));
  });

  test('pen flags follow the device kind and barrel button', () {
    expect(windowsPenFlags(PointerDeviceKind.stylus, 0), 0);
    expect(
      windowsPenFlags(PointerDeviceKind.stylus, kPrimaryStylusButton),
      ViewerPenFlag.barrel,
    );
    expect(
      windowsPenFlags(PointerDeviceKind.invertedStylus, 0),
      ViewerPenFlag.inverted | ViewerPenFlag.eraser,
    );
    expect(
      windowsPenFlags(PointerDeviceKind.invertedStylus, kPrimaryStylusButton),
      ViewerPenFlag.inverted | ViewerPenFlag.eraser | ViewerPenFlag.barrel,
    );
    expect(windowsPenFlags(PointerDeviceKind.touch, kPrimaryButton), 0);
  });

  test('mouse mapping is a no-op when both switches are off', () {
    const MouseMapping mapping = MouseMapping();
    expect(mapping.buttons(ViewerMouse.left), ViewerMouse.left);
    expect(mapping.buttons(ViewerMouse.right), ViewerMouse.right);
    expect(
      mapping.buttons(ViewerMouse.left | ViewerMouse.middle),
      ViewerMouse.left | ViewerMouse.middle,
    );
    expect(mapping.wheelY(120), 120);
    expect(mapping.wheelX(-120), -120);
  });

  test('swapping exchanges left and right and keeps the other buttons', () {
    const MouseMapping mapping = MouseMapping(swapButtons: true);
    expect(mapping.buttons(ViewerMouse.left), ViewerMouse.right);
    expect(mapping.buttons(ViewerMouse.right), ViewerMouse.left);
    expect(mapping.buttons(ViewerMouse.left | ViewerMouse.right), 3);
    expect(
      mapping.buttons(
        ViewerMouse.left | ViewerMouse.middle | ViewerMouse.back,
      ),
      ViewerMouse.right | ViewerMouse.middle | ViewerMouse.back,
    );
    expect(mapping.buttons(0), 0);
  });

  test('reversing the wheel flips both axes', () {
    const MouseMapping mapping = MouseMapping(reverseWheel: true);
    expect(mapping.wheelY(120), -120);
    expect(mapping.wheelY(-120), 120);
    expect(mapping.wheelX(240), -240);
    expect(mapping.buttons(ViewerMouse.left), ViewerMouse.left);
  });
}
