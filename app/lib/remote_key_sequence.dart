/// One synthetic key event: the HID usage, whether it goes down, and the
/// modifier bitmap that travels with it.
class RemoteKeyEvent {
  const RemoteKeyEvent(this.usage, this.down, this.modifiers);

  final int usage;
  final bool down;
  final int modifiers;
}

const int kHidLeftGui = 0xE3;

int _modifierBit(int usage) => 1 << (usage - 0xE0);

/// Expands a toolbar shortcut into the press/release sequence a physical
/// chord produces: every modifier down in order (the bitmap gains its bit
/// before the event that carries it), the key down and up, then the modifiers
/// released in reverse. [baseModifiers] keeps whatever this keyboard already
/// holds.
List<RemoteKeyEvent> buildRemoteChord(
    List<int> modifierUsages, int keyUsage, int baseModifiers) {
  final List<RemoteKeyEvent> events = <RemoteKeyEvent>[];
  int bits = baseModifiers;
  for (final int usage in modifierUsages) {
    bits |= _modifierBit(usage);
    events.add(RemoteKeyEvent(usage, true, bits));
  }
  events.add(RemoteKeyEvent(keyUsage, true, bits));
  events.add(RemoteKeyEvent(keyUsage, false, bits));
  for (final int usage in modifierUsages.reversed) {
    bits &= ~_modifierBit(usage);
    events.add(RemoteKeyEvent(usage, false, bits));
  }
  return events;
}

/// A Win tap. Win is a modifier itself, so the key-up clears its own bitmap
/// bit instead of leaving a phantom gui-down behind.
List<RemoteKeyEvent> buildRemoteWinTap(int baseModifiers, int guiBit) {
  final int downBits = baseModifiers | guiBit;
  return <RemoteKeyEvent>[
    RemoteKeyEvent(kHidLeftGui, true, downBits),
    RemoteKeyEvent(kHidLeftGui, false, downBits & ~guiBit),
  ];
}
