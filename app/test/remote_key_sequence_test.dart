import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/remote_key_sequence.dart';

void main() {
  test('a chord presses modifiers first and releases them last', () {
    final List<RemoteKeyEvent> events =
        buildRemoteChord(<int>[0xE2], 0x2B, 0);

    expect(events.map((RemoteKeyEvent e) => e.usage).toList(),
        <int>[0xE2, 0x2B, 0x2B, 0xE2]);
    expect(events.map((RemoteKeyEvent e) => e.down).toList(),
        <bool>[true, true, false, false]);
    expect(events.map((RemoteKeyEvent e) => e.modifiers).toList(),
        <int>[0x04, 0x04, 0x04, 0x00]);
  });

  test('two modifiers stack their bits and release in reverse', () {
    final List<RemoteKeyEvent> events =
        buildRemoteChord(<int>[0xE0, 0xE1], 0x29, 0x08);

    expect(events.map((RemoteKeyEvent e) => e.usage).toList(),
        <int>[0xE0, 0xE1, 0x29, 0x29, 0xE1, 0xE0]);
    expect(events.map((RemoteKeyEvent e) => e.modifiers).toList(),
        <int>[0x09, 0x0B, 0x0B, 0x0B, 0x09, 0x08]);
    expect(events.map((RemoteKeyEvent e) => e.down).toList(),
        <bool>[true, true, true, false, false, false]);
  });

  test('a Win tap clears its own bit before the key-up', () {
    final List<RemoteKeyEvent> events = buildRemoteWinTap(0x01, 0x08);

    expect(events.length, 2);
    expect(events[0].usage, kHidLeftGui);
    expect(events[0].down, isTrue);
    expect(events[0].modifiers, 0x09);
    expect(events[1].down, isFalse);
    expect(events[1].modifiers, 0x01);
  });
}
