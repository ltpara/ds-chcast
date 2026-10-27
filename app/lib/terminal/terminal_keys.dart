import 'package:flutter/services.dart';

/// Translates a Flutter key event into the bytes a VT console expects.
///
/// Returns null when the event is not console input (for example a bare
/// modifier press, or Ctrl+Shift+C/V which the panel keeps for copy/paste).
/// Printable characters are not covered here: the panel reads
/// [KeyEvent.character] itself so dead keys and layouts work.
String? terminalKeySequence(KeyEvent event) {
  final HardwareKeyboard keyboard = HardwareKeyboard.instance;
  final bool ctrl = keyboard.isControlPressed;
  final bool shift = keyboard.isShiftPressed;
  final bool alt = keyboard.isAltPressed;
  final LogicalKeyboardKey key = event.logicalKey;

  // xterm modifier parameter: 1 + shift + 2*alt + 4*ctrl.
  final int modifier = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);

  if (key == LogicalKeyboardKey.enter || key == LogicalKeyboardKey.numpadEnter) {
    return '\r';
  }
  if (key == LogicalKeyboardKey.backspace) {
    return '\x7f';
  }
  if (key == LogicalKeyboardKey.tab) {
    return shift ? '\x1b[Z' : '\t';
  }
  if (key == LogicalKeyboardKey.escape) {
    return '\x1b';
  }
  if (key == LogicalKeyboardKey.arrowUp) {
    return _csi('A', modifier);
  }
  if (key == LogicalKeyboardKey.arrowDown) {
    return _csi('B', modifier);
  }
  if (key == LogicalKeyboardKey.arrowRight) {
    return _csi('C', modifier);
  }
  if (key == LogicalKeyboardKey.arrowLeft) {
    return _csi('D', modifier);
  }
  if (key == LogicalKeyboardKey.home) {
    return _csi('H', modifier);
  }
  if (key == LogicalKeyboardKey.end) {
    return _csi('F', modifier);
  }
  if (key == LogicalKeyboardKey.insert) {
    return _tilde(2, modifier);
  }
  if (key == LogicalKeyboardKey.delete) {
    return _tilde(3, modifier);
  }
  if (key == LogicalKeyboardKey.pageUp) {
    return _tilde(5, modifier);
  }
  if (key == LogicalKeyboardKey.pageDown) {
    return _tilde(6, modifier);
  }
  if (key == LogicalKeyboardKey.f1) {
    return _ss3('P', modifier);
  }
  if (key == LogicalKeyboardKey.f2) {
    return _ss3('Q', modifier);
  }
  if (key == LogicalKeyboardKey.f3) {
    return _ss3('R', modifier);
  }
  if (key == LogicalKeyboardKey.f4) {
    return _ss3('S', modifier);
  }
  const List<int> tildeKeys = <int>[
    15, // F5
    17, // F6
    18, // F7
    19, // F8
    20, // F9
    21, // F10
    23, // F11
    24, // F12
  ];
  const List<LogicalKeyboardKey> functionKeys = <LogicalKeyboardKey>[
    LogicalKeyboardKey.f5,
    LogicalKeyboardKey.f6,
    LogicalKeyboardKey.f7,
    LogicalKeyboardKey.f8,
    LogicalKeyboardKey.f9,
    LogicalKeyboardKey.f10,
    LogicalKeyboardKey.f11,
    LogicalKeyboardKey.f12,
  ];
  final int functionIndex = functionKeys.indexOf(key);
  if (functionIndex >= 0) {
    return _tilde(tildeKeys[functionIndex], modifier);
  }
  if (ctrl && !alt) {
    if (key == LogicalKeyboardKey.space) {
      return '\x00';
    }
    final String label = key.keyLabel.toUpperCase();
    if (label.length == 1) {
      final int code = label.codeUnitAt(0);
      if (code >= 0x40 && code <= 0x5F) {
        // Ctrl+Shift+C/V stay with the panel for clipboard work.
        if (shift && (code == 0x43 || code == 0x56)) {
          return null;
        }
        return String.fromCharCode(code & 0x1F);
      }
    }
  }
  return null;
}

String _csi(String finalByte, int modifier) {
  return modifier <= 1 ? '\x1b[$finalByte' : '\x1b[1;$modifier$finalByte';
}

String _tilde(int number, int modifier) {
  return modifier <= 1 ? '\x1b[$number~' : '\x1b[$number;$modifier~';
}

String _ss3(String finalByte, int modifier) {
  return modifier <= 1 ? '\x1bO$finalByte' : '\x1b[1;$modifier$finalByte';
}
