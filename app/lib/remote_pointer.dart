import 'dart:math' as math;

import 'package:flutter/gestures.dart';

import 'viewer_bridge.dart';

/// Maps Flutter pointer ids to the contact slots the protocol and host
/// injector accept. A slot stays assigned until its contact lifts, so the
/// host can match moves and the lift to the original press.
class RemoteContactTracker {
  final Map<int, int> _slots = <int, int>{};

  int? slotFor(int pointer) => _slots[pointer];

  Iterable<int> get contacts => _slots.values;

  /// Returns [pointer]'s slot, allocating the lowest free one. Null when all
  /// [kViewerMaxTouchContacts] slots are taken; the caller drops that contact
  /// rather than reusing another one's slot.
  int? acquire(int pointer) {
    final int? existing = _slots[pointer];
    if (existing != null) {
      return existing;
    }
    for (int slot = 0; slot < kViewerMaxTouchContacts; slot++) {
      if (!_slots.containsValue(slot)) {
        _slots[pointer] = slot;
        return slot;
      }
    }
    return null;
  }

  int? release(int pointer) => _slots.remove(pointer);

  void clear() => _slots.clear();
}

/// Flutter pressure -> Windows' 0..1024 range, shared by pen and touch. Zero
/// means "not reported" (a default pressure with no range, or a non-finite
/// value); the host then omits the pressure field instead of inventing a
/// value.
int windowsPointerPressure(double pressure, double min, double max) {
  if (!pressure.isFinite || !min.isFinite || !max.isFinite || max <= min) {
    return 0;
  }
  final double normalized = ((pressure - min) / (max - min)).clamp(0.0, 1.0);
  return (normalized * 1024).round();
}

/// Flutter reports a stylus as an angle from the surface normal ([tilt],
/// 0 = upright) plus an azimuth ([orientation], 0 = toward the top of the
/// screen, positive clockwise). Windows wants degrees along each axis,
/// positive right / toward the user.
(int, int) windowsPenTilt(double tilt, double orientation) {
  if (!tilt.isFinite || !orientation.isFinite) {
    return (0, 0);
  }
  final double degrees = tilt * 180 / math.pi;
  final int x =
      (degrees * math.sin(orientation)).round().clamp(-90, 90).toInt();
  final int y =
      (-degrees * math.cos(orientation)).round().clamp(-90, 90).toInt();
  return (x, y);
}

/// Pen flags for a Flutter event. The eraser end is both inverted and an
/// eraser to Windows; a pressed stylus barrel button maps to the barrel flag.
/// [buttons] is `PointerEvent.buttons`.
int windowsPenFlags(PointerDeviceKind kind, int buttons) {
  int flags = 0;
  if (kind == PointerDeviceKind.invertedStylus) {
    flags |= ViewerPenFlag.inverted | ViewerPenFlag.eraser;
  }
  if (buttons & kPrimaryStylusButton != 0) {
    flags |= ViewerPenFlag.barrel;
  }
  return flags;
}
