import 'package:chast_viewer/terminal/terminal_keys.dart';
import 'package:chast_viewer/terminal/terminal_model.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('TerminalGrid', () {
    test('prints text and advances the cursor', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 3);
      grid.write('hello');
      expect(grid.lineText(0), 'hello');
      expect(grid.cursorRow, 0);
      expect(grid.cursorColumn, 5);
    });

    test('CR and LF move the cursor and scroll into history', () {
      final TerminalGrid grid = TerminalGrid(columns: 8, rows: 2);
      grid.write('a\r\nb\r\nc');
      expect(grid.lineText(0), 'b');
      expect(grid.lineText(1), 'c');
      expect(grid.scrollback.length, 1);
      expect(_lineText(grid.scrollback.first), 'a');
    });

    test('autowrap holds the cursor at the last column until the next glyph',
        () {
      final TerminalGrid grid = TerminalGrid(columns: 3, rows: 2);
      grid.write('abc');
      expect(grid.lineText(0), 'abc');
      expect(grid.cursorColumn, 2);
      expect(grid.wrapPending, isTrue);
      grid.write('d');
      expect(grid.lineText(0), 'abc');
      expect(grid.lineText(1), 'd');
      expect(grid.cursorColumn, 1);
    });

    test('cursor addressing and erase', () {
      final TerminalGrid grid = TerminalGrid(columns: 8, rows: 4);
      grid.write('hello\r\nworld');
      grid.write('\x1b[2;1H');
      expect(grid.cursorRow, 1);
      expect(grid.cursorColumn, 0);
      grid.write('\x1b[2K');
      expect(grid.lineText(1), '');
      grid.write('ab');
      expect(grid.lineText(1), 'ab');
      grid.write('\x1b[2J');
      expect(grid.lineText(0), '');
      expect(grid.lineText(1), '');
      expect(grid.lineText(2), '');
    });

    test('SGR sets attributes and indexed colours', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 2);
      grid.write('\x1b[1;31;44mX');
      final TerminalCell cell = grid.lines[0][0];
      expect(cell.text, 'X');
      expect(cell.attrs & TerminalAttr.bold, TerminalAttr.bold);
      expect(cell.fg, 1);
      expect(cell.bg, 4);
      grid.write('\x1b[0mY');
      expect(grid.lines[0][1].fg, kTerminalDefaultColor);
      expect(grid.lines[0][1].attrs, 0);
    });

    test('SGR parses 256-colour and true colour', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 2);
      grid.write('\x1b[38;5;196mA');
      expect(grid.lines[0][0].fg, 196);
      grid.write('\x1b[38;2;1;2;3mB');
      expect(grid.lines[0][1].fg, kTerminalRgbFlag | 0x010203);
    });

    test('a wide character owns two cells', () {
      final TerminalGrid grid = TerminalGrid(columns: 6, rows: 2);
      grid.write('中a');
      final List<TerminalCell> line = grid.lines[0];
      expect(line[0].text, '中');
      expect(line[1].continuation, isTrue);
      expect(line[2].text, 'a');
      expect(grid.cursorColumn, 3);
      expect(grid.lineText(0), '中a');
    });

    test('overwriting half of a wide character clears the other half', () {
      final TerminalGrid grid = TerminalGrid(columns: 6, rows: 2);
      grid.write('中');
      grid.write('\x1b[1Gx');
      expect(grid.lines[0][0].text, 'x');
      expect(grid.lines[0][1].continuation, isFalse);
      expect(grid.lines[0][1].text, ' ');
    });

    test('OSC sets the window title', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 2);
      grid.write('\x1b]0;远程终端\x07');
      expect(grid.title, '远程终端');
    });

    test('scroll region confines line feeds', () {
      final TerminalGrid grid = TerminalGrid(columns: 4, rows: 5);
      grid.write('\x1b[2;3r');
      expect(grid.scrollTop, 1);
      expect(grid.scrollBottom, 2);
      grid.write('\x1b[2;1Hx\x1b[3;1Hy\r\nz');
      expect(grid.lineText(0), '');
      expect(grid.lineText(1), 'y');
      expect(grid.lineText(2), 'z');
    });

    test('the alternate screen is restored on exit', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 3);
      grid.write('main');
      grid.write('\x1b[?1049h');
      expect(grid.altScreen, isTrue);
      grid.write('alt');
      expect(grid.lineText(0), 'alt');
      grid.write('\x1b[?1049l');
      expect(grid.altScreen, isFalse);
      expect(grid.lineText(0), 'main');
    });

    test('private modes toggle cursor and paste flags', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 2);
      grid.write('\x1b[?25l');
      expect(grid.cursorVisible, isFalse);
      grid.write('\x1b[?25h');
      expect(grid.cursorVisible, isTrue);
      grid.write('\x1b[?2004h');
      expect(grid.bracketedPaste, isTrue);
      grid.write('\x1b[?7l');
      expect(grid.autoWrap, isFalse);
    });

    test('device status reports go back through onReply', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 3);
      final List<String> replies = <String>[];
      grid.onReply = replies.add;
      grid.write('\x1b[2;3H');
      grid.write('\x1b[6n');
      expect(replies, <String>['\x1b[2;3R']);
      grid.write('\x1b[5n');
      expect(replies.last, '\x1b[0n');
    });

    test('resize clips rows and keeps history', () {
      final TerminalGrid grid = TerminalGrid(columns: 4, rows: 3);
      grid.write('ab\r\ncd');
      grid.resize(6, 2);
      expect(grid.columns, 6);
      expect(grid.rows, 2);
      expect(grid.scrollback.length, 1);
      expect(_lineText(grid.scrollback.first), 'ab');
      expect(grid.lineText(0), 'cd');
      expect(grid.lines[0].length, 6);
    });

    test('resize grows and pads the current rows', () {
      final TerminalGrid grid = TerminalGrid(columns: 4, rows: 2);
      grid.write('x');
      grid.resize(2, 4);
      expect(grid.lines.length, 4);
      expect(grid.lines[0].length, 2);
      expect(grid.lineText(0), 'x');
      expect(grid.cursorColumn, 1);
    });

    test('charset designators do not swallow following text', () {
      final TerminalGrid grid = TerminalGrid(columns: 10, rows: 2);
      grid.write('\x1b(0abc');
      expect(grid.lineText(0), 'abc');
    });
  });

  group('TerminalUtf8Decoder', () {
    test('reassembles a sequence split across chunks', () {
      final TerminalUtf8Decoder decoder = TerminalUtf8Decoder();
      expect(decoder.add(<int>[0xE4, 0xB8]), '');
      expect(decoder.add(<int>[0xAD]), '中');
    });

    test('replaces malformed bytes without dropping the rest', () {
      final TerminalUtf8Decoder decoder = TerminalUtf8Decoder();
      expect(decoder.add(<int>[0xFF, 0x41]), '\uFFFDA');
    });

    test('keeps a partial tail after complete text', () {
      final TerminalUtf8Decoder decoder = TerminalUtf8Decoder();
      expect(decoder.add(<int>[0x41, 0xE4]), 'A');
      expect(decoder.add(<int>[0xB8, 0xAD]), '中');
    });
  });

  group('terminalKeySequence', () {
    testWidgets('maps navigation and function keys', (WidgetTester tester) async {
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.arrowUp)),
        '\x1b[A',
      );
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.arrowLeft)),
        '\x1b[D',
      );
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.home)),
        '\x1b[H',
      );
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.delete)),
        '\x1b[3~',
      );
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.f1)),
        '\x1bOP',
      );
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.f11)),
        '\x1b[23~',
      );
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.enter)),
        '\r',
      );
      expect(
        terminalKeySequence(_event(LogicalKeyboardKey.backspace)),
        '\x7f',
      );
    });

    testWidgets('maps Ctrl chords to control bytes', (WidgetTester tester) async {
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      expect(terminalKeySequence(_event(LogicalKeyboardKey.keyC)), '\x03');
      expect(terminalKeySequence(_event(LogicalKeyboardKey.keyD)), '\x04');
      expect(terminalKeySequence(_event(LogicalKeyboardKey.keyZ)), '\x1a');
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    });

    testWidgets('Ctrl+Shift+C stays with the panel', (WidgetTester tester) async {
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
      expect(terminalKeySequence(_event(LogicalKeyboardKey.keyC)), isNull);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    });

    testWidgets('Shift+Tab sends the back-tab sequence', (WidgetTester tester) async {
      await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
      expect(terminalKeySequence(_event(LogicalKeyboardKey.tab)), '\x1b[Z');
      await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
    });

    test('plain characters are left to the panel', () {
      expect(terminalKeySequence(_event(LogicalKeyboardKey.keyA, character: 'a')),
          isNull);
    });
  });
}

KeyDownEvent _event(LogicalKeyboardKey key, {String? character}) {
  return KeyDownEvent(
    physicalKey: PhysicalKeyboardKey.keyA,
    logicalKey: key,
    character: character,
    timeStamp: Duration.zero,
  );
}

String _lineText(List<TerminalCell> line) {
  final StringBuffer out = StringBuffer();
  for (final TerminalCell cell in line) {
    if (cell.continuation) {
      continue;
    }
    out.write(cell.text);
  }
  return out.toString().replaceFirst(RegExp(r' +$'), '');
}
