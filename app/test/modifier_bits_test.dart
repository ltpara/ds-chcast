import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('maps every modifier usage to its bit', () {
    expect(modifierBitsFor(const <PhysicalKeyboardKey>[]), 0);
    final Map<PhysicalKeyboardKey, int> expected = <PhysicalKeyboardKey, int>{
      PhysicalKeyboardKey.controlLeft: ViewerModifier.leftCtrl,
      PhysicalKeyboardKey.shiftLeft: ViewerModifier.leftShift,
      PhysicalKeyboardKey.altLeft: ViewerModifier.leftAlt,
      PhysicalKeyboardKey.metaLeft: ViewerModifier.leftGui,
      PhysicalKeyboardKey.controlRight: ViewerModifier.rightCtrl,
      PhysicalKeyboardKey.shiftRight: ViewerModifier.rightShift,
      PhysicalKeyboardKey.altRight: ViewerModifier.rightAlt,
      PhysicalKeyboardKey.metaRight: ViewerModifier.rightGui,
    };
    expected.forEach((PhysicalKeyboardKey key, int bit) {
      expect(modifierBitsFor(<PhysicalKeyboardKey>[key]), bit, reason: '$key');
    });
    expect(modifierBitsFor(expected.keys), ViewerModifier.mask);
    expect(modifierBitsFor(<PhysicalKeyboardKey>[PhysicalKeyboardKey.keyA]), 0);
  });

  test('maps lock state to its bits', () {
    expect(lockBitsFor(const <KeyboardLockMode>{}), 0);
    expect(lockBitsFor(<KeyboardLockMode>{KeyboardLockMode.capsLock}),
        ViewerLock.caps);
    expect(lockBitsFor(<KeyboardLockMode>{KeyboardLockMode.numLock}),
        ViewerLock.num);
    expect(lockBitsFor(<KeyboardLockMode>{KeyboardLockMode.scrollLock}),
        ViewerLock.scroll);
    expect(
        lockBitsFor(<KeyboardLockMode>{
          KeyboardLockMode.capsLock,
          KeyboardLockMode.numLock,
          KeyboardLockMode.scrollLock,
        }),
        ViewerLock.mask);
  });

  testWidgets('follows the keyboard state Flutter reports', (WidgetTester tester) async {
    await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft,
        physicalKey: PhysicalKeyboardKey.shiftLeft);
    expect(modifierBitsFor(HardwareKeyboard.instance.physicalKeysPressed),
        ViewerModifier.leftShift);

    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft,
        physicalKey: PhysicalKeyboardKey.controlLeft);
    expect(modifierBitsFor(HardwareKeyboard.instance.physicalKeysPressed),
        ViewerModifier.leftShift | ViewerModifier.leftCtrl);

    await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft,
        physicalKey: PhysicalKeyboardKey.shiftLeft);
    expect(modifierBitsFor(HardwareKeyboard.instance.physicalKeysPressed),
        ViewerModifier.leftCtrl);

    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft,
        physicalKey: PhysicalKeyboardKey.controlLeft);
    expect(modifierBitsFor(HardwareKeyboard.instance.physicalKeysPressed), 0);
  });
}
