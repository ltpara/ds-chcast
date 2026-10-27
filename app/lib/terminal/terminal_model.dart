/// Screen and parser for the viewer's remote console.
///
/// The host runs `%COMSPEC%` under ConPTY, so the byte stream is VT/ANSI.
/// This model covers the subset console applications actually emit: C0
/// controls, cursor addressing, erase, insert/delete, scrolling regions,
/// SGR, the alternate screen and window titles. It keeps no transport state;
/// the panel feeds decoded text in and reads the grid out.
library;

/// The cell's colour is the terminal default (SGR 39/49 or a reset).
const int kTerminalDefaultColor = -1;

/// Flag on a colour value that carries 24-bit RGB instead of a palette index,
/// for example `kTerminalRgbFlag | 0x1F6FEB`.
const int kTerminalRgbFlag = 0x1000000;

/// Attribute bits stored on every cell.
abstract final class TerminalAttr {
  static const int bold = 1;
  static const int dim = 2;
  static const int italic = 4;
  static const int underline = 8;
  static const int inverse = 16;
  static const int hidden = 32;
}

/// One character cell. A double-width character owns its cell and marks the
/// next one as [continuation]; the renderer skips continuations.
class TerminalCell {
  TerminalCell();

  String text = ' ';
  int fg = kTerminalDefaultColor;
  int bg = kTerminalDefaultColor;
  int attrs = 0;
  bool continuation = false;

  void set(String value, int foreground, int background, int attributes) {
    text = value;
    fg = foreground;
    bg = background;
    attrs = attributes;
    continuation = false;
  }

  void clearTo(int foreground, int background) {
    text = ' ';
    fg = foreground;
    bg = background;
    attrs = 0;
    continuation = false;
  }
}

/// Turns a byte stream into text while tolerating chunk boundaries: a UTF-8
/// sequence split across two pieces is held back until the rest arrives.
class TerminalUtf8Decoder {
  final List<int> _pending = <int>[];

  /// Decodes what [bytes] completes and keeps a partial tail for the next
  /// call. Malformed sequences become U+FFFD without dropping the rest.
  String add(List<int> bytes) {
    _pending.addAll(bytes);
    final StringBuffer out = StringBuffer();
    int i = 0;
    while (i < _pending.length) {
      final int first = _pending[i];
      if (first < 0x80) {
        out.writeCharCode(first);
        i++;
        continue;
      }
      int extra;
      int minimum;
      int code;
      if (first >= 0xC2 && first <= 0xDF) {
        extra = 1;
        minimum = 0x80;
        code = first & 0x1F;
      } else if (first >= 0xE0 && first <= 0xEF) {
        extra = 2;
        minimum = 0x800;
        code = first & 0x0F;
      } else if (first >= 0xF0 && first <= 0xF4) {
        extra = 3;
        minimum = 0x10000;
        code = first & 0x07;
      } else {
        out.writeCharCode(0xFFFD);
        i++;
        continue;
      }
      if (i + extra >= _pending.length) {
        // Not enough bytes yet; the rest of the sequence is in the next chunk.
        break;
      }
      bool valid = true;
      for (int k = 1; k <= extra; k++) {
        final int next = _pending[i + k];
        if ((next & 0xC0) != 0x80) {
          valid = false;
          break;
        }
        code = (code << 6) | (next & 0x3F);
      }
      if (!valid ||
          code < minimum ||
          code > 0x10FFFF ||
          (code >= 0xD800 && code <= 0xDFFF)) {
        out.writeCharCode(0xFFFD);
        i++;
        continue;
      }
      out.writeCharCode(code);
      i += extra + 1;
    }
    if (i > 0) {
      _pending.removeRange(0, i);
    }
    return out.toString();
  }

  void reset() => _pending.clear();
}

/// Wide (double-cell) code points for the common east-asian and emoji ranges.
bool isWideRune(int rune) {
  if (rune < 0x1100) {
    return false;
  }
  return (rune >= 0x1100 && rune <= 0x115F) ||
      (rune >= 0x2E80 && rune <= 0x303E) ||
      (rune >= 0x3041 && rune <= 0x33FF) ||
      (rune >= 0x3400 && rune <= 0x4DBF) ||
      (rune >= 0x4E00 && rune <= 0x9FFF) ||
      (rune >= 0xA000 && rune <= 0xA4CF) ||
      (rune >= 0xA960 && rune <= 0xA97F) ||
      (rune >= 0xAC00 && rune <= 0xD7A3) ||
      (rune >= 0xF900 && rune <= 0xFAFF) ||
      (rune >= 0xFE10 && rune <= 0xFE19) ||
      (rune >= 0xFE30 && rune <= 0xFE6F) ||
      (rune >= 0xFF00 && rune <= 0xFF60) ||
      (rune >= 0xFFE0 && rune <= 0xFFE6) ||
      (rune >= 0x1F300 && rune <= 0x1F64F) ||
      (rune >= 0x1F900 && rune <= 0x1F9FF) ||
      (rune >= 0x20000 && rune <= 0x3FFFD);
}

enum _State { ground, escape, csi, osc, oscEscape, dcs, dcsEscape, charset }

/// One console screen: a grid of [TerminalCell]s, a cursor, the current
/// rendition and the parser that mutates them.
class TerminalGrid {
  TerminalGrid({
    int columns = 80,
    int rows = 24,
    this.maxScrollback = 2000,
  })  : _columns = columns.clamp(1, 500),
        _rows = rows.clamp(1, 200) {
    _lines = _blankLines(_rows, _columns);
    scrollTop = 0;
    scrollBottom = _rows - 1;
  }

  final int maxScrollback;

  int _columns;
  int _rows;
  late List<List<TerminalCell>> _lines;
  final List<List<TerminalCell>> _scrollback = <List<TerminalCell>>[];

  int get columns => _columns;
  int get rows => _rows;

  /// The visible screen, top row first. Rows are exactly [columns] long.
  List<List<TerminalCell>> get lines => _lines;

  /// Lines that scrolled off the top, oldest first.
  List<List<TerminalCell>> get scrollback => _scrollback;

  int cursorRow = 0;
  int cursorColumn = 0;
  bool cursorVisible = true;
  bool autoWrap = true;
  bool appCursorKeys = false;
  bool bracketedPaste = false;
  int scrollTop = 0;
  int scrollBottom = 23;
  String title = '';
  int bellCount = 0;

  /// Answers the host's device-status queries (DSR); the panel wires this to
  /// `sendTerminalInput` so `\x1b[6n` gets a cursor report back.
  void Function(String reply)? onReply;

  int fg = kTerminalDefaultColor;
  int bg = kTerminalDefaultColor;
  int attrs = 0;

  bool _wrapPending = false;
  bool _altActive = false;
  List<List<TerminalCell>>? _savedScreen;
  int _savedRow = 0;
  int _savedColumn = 0;
  int _savedFg = kTerminalDefaultColor;
  int _savedBg = kTerminalDefaultColor;
  int _savedAttrs = 0;
  bool _savedWrap = true;
  int _savedScrollTop = 0;
  int _savedScrollBottom = 0;

  _State _state = _State.ground;
  final StringBuffer _csiBody = StringBuffer();
  final StringBuffer _oscBody = StringBuffer();

  /// True while the last printed character sits in the final column and the
  /// next printable character starts a new line.
  bool get wrapPending => _wrapPending;

  bool get altScreen => _altActive;

  /// Feeds decoded text. Callers decode with [TerminalUtf8Decoder] first.
  void write(String text) {
    for (final int rune in text.runes) {
      _feed(rune);
    }
  }

  void reset() {
    _scrollback.clear();
    _lines = _blankLines(_rows, _columns);
    cursorRow = 0;
    cursorColumn = 0;
    cursorVisible = true;
    autoWrap = true;
    appCursorKeys = false;
    bracketedPaste = false;
    scrollTop = 0;
    scrollBottom = _rows - 1;
    fg = kTerminalDefaultColor;
    bg = kTerminalDefaultColor;
    attrs = 0;
    _wrapPending = false;
    _altActive = false;
    _savedScreen = null;
    title = '';
    _state = _State.ground;
    _csiBody.clear();
    _oscBody.clear();
  }

  /// Changes the grid to [columns] x [rows]. Content is clipped or padded;
  /// rows that leave the top move into the scrollback when the whole screen
  /// scrolls, so history survives a window that got shorter.
  void resize(int columns, int rows) {
    final int newColumns = columns.clamp(1, 500);
    final int newRows = rows.clamp(1, 200);
    if (newColumns == _columns && newRows == _rows) {
      return;
    }
    final int oldRows = _rows;
    final int dropped = oldRows - newRows;
    _columns = newColumns;
    if (dropped > 0) {
      if (!_altActive) {
        for (int i = 0; i < dropped && i < _lines.length; i++) {
          _pushScrollback(_lines[i]);
        }
      }
      _lines.removeRange(0, dropped);
      cursorRow = cursorRow - dropped;
    } else if (dropped < 0) {
      for (int i = 0; i < -dropped; i++) {
        _lines.add(_blankLine());
      }
    }
    _rows = newRows;
    for (final List<TerminalCell> line in _lines) {
      _resizeLine(line);
    }
    final List<List<TerminalCell>>? saved = _savedScreen;
    if (saved != null) {
      for (final List<TerminalCell> line in saved) {
        _resizeLine(line);
      }
    }
    scrollTop = 0;
    scrollBottom = _rows - 1;
    cursorRow = cursorRow.clamp(0, _rows - 1);
    cursorColumn = cursorColumn.clamp(0, _columns - 1);
    _wrapPending = false;
  }

  /// The row as text, right-trimmed unless [trim] is false.
  String lineText(int row, {bool trim = true}) {
    final StringBuffer out = StringBuffer();
    for (final TerminalCell cell in _lines[row]) {
      if (cell.continuation) {
        continue;
      }
      out.write(cell.text);
    }
    final String text = out.toString();
    if (!trim) {
      return text;
    }
    int end = text.length;
    while (end > 0 && text.codeUnitAt(end - 1) == 0x20) {
      end--;
    }
    return text.substring(0, end);
  }

  /// The whole screen as text, one line per row.
  String get screenText {
    final StringBuffer out = StringBuffer();
    for (int row = 0; row < _rows; row++) {
      if (row > 0) {
        out.write('\n');
      }
      out.write(lineText(row));
    }
    return out.toString();
  }

  // ---------------------------------------------------------------- parser

  void _feed(int rune) {
    switch (_state) {
      case _State.ground:
        _ground(rune);
      case _State.escape:
        _escape(rune);
      case _State.csi:
        _csi(rune);
      case _State.osc:
        _osc(rune);
      case _State.oscEscape:
        if (rune == 0x5C) {
          _commitOsc();
          _state = _State.ground;
        } else {
          _state = _State.osc;
          _osc(rune);
        }
      case _State.dcs:
        if (rune == 0x07) {
          _state = _State.ground;
        } else if (rune == 0x1B) {
          _state = _State.dcsEscape;
        }
      case _State.dcsEscape:
        if (rune == 0x5C) {
          _state = _State.ground;
        } else {
          _state = _State.dcs;
        }
      case _State.charset:
        _state = _State.ground;
    }
  }

  void _ground(int rune) {
    if (rune == 0x1B) {
      _state = _State.escape;
      return;
    }
    if (rune < 0x20) {
      _control(rune);
      return;
    }
    if (rune == 0x7F) {
      return;
    }
    if (rune == 0x9B) {
      _beginCsi();
      return;
    }
    if (rune == 0x9D) {
      _beginOsc();
      return;
    }
    _print(rune);
  }

  void _control(int rune) {
    switch (rune) {
      case 0x07:
        bellCount++;
      case 0x08:
        _backspace();
      case 0x09:
        _tab(1);
      case 0x0A:
      case 0x0B:
      case 0x0C:
        _wrapPending = false;
        _lineFeed();
      case 0x0D:
        cursorColumn = 0;
        _wrapPending = false;
      case 0x0E:
      case 0x0F:
        break;
    }
  }

  void _print(int rune) {
    if (_wrapPending) {
      _wrapPending = false;
      if (autoWrap) {
        cursorColumn = 0;
        _lineFeed();
      }
    }
    bool wide = isWideRune(rune);
    if (wide && cursorColumn >= _columns - 1) {
      if (autoWrap) {
        cursorColumn = 0;
        _lineFeed();
      } else {
        wide = false;
      }
    }
    final List<TerminalCell> line = _lines[cursorRow];
    _clearWideAt(line, cursorColumn);
    line[cursorColumn].set(String.fromCharCode(rune), fg, bg, attrs);
    if (wide) {
      _clearWideAt(line, cursorColumn + 1);
      final TerminalCell tail = line[cursorColumn + 1];
      tail.set(' ', fg, bg, attrs);
      tail.continuation = true;
    }
    final int width = wide ? 2 : 1;
    if (cursorColumn + width >= _columns) {
      cursorColumn = _columns - 1;
      _wrapPending = autoWrap;
    } else {
      cursorColumn += width;
    }
  }

  /// Clears [line] at [column]; if that cell is half of a double-width
  /// character, the other half is cleared too so no orphan glyph remains.
  void _clearWideAt(List<TerminalCell> line, int column) {
    final TerminalCell cell = line[column];
    if (cell.continuation && column > 0) {
      line[column - 1].clearTo(fg, bg);
    } else if (column + 1 < _columns && line[column + 1].continuation) {
      line[column + 1].clearTo(fg, bg);
    }
  }

  void _backspace() {
    _wrapPending = false;
    if (cursorColumn > 0) {
      cursorColumn--;
      if (_lines[cursorRow][cursorColumn].continuation && cursorColumn > 0) {
        cursorColumn--;
      }
    }
  }

  void _tab(int count) {
    _wrapPending = false;
    for (int i = 0; i < count; i++) {
      final int next = (cursorColumn ~/ 8 + 1) * 8;
      cursorColumn = next < _columns ? next : _columns - 1;
    }
  }

  void _beginCsi() {
    _csiBody.clear();
    _state = _State.csi;
  }

  void _beginOsc() {
    _oscBody.clear();
    _state = _State.osc;
  }

  void _escape(int rune) {
    _state = _State.ground;
    switch (rune) {
      case 0x5B: // [
        _beginCsi();
      case 0x5D: // ]
        _beginOsc();
      case 0x50: // P - DCS
      case 0x58: // X - SOS
      case 0x5E: // ^ - PM
      case 0x5F: // _ - APC
        _state = _State.dcs;
      case 0x37: // 7
        _saveCursor();
      case 0x38: // 8
        _restoreCursor();
      case 0x44: // D
        _wrapPending = false;
        _lineFeed();
      case 0x4D: // M
        _wrapPending = false;
        _reverseIndex();
      case 0x45: // E
        cursorColumn = 0;
        _wrapPending = false;
        _lineFeed();
      case 0x63: // c
        reset();
      case 0x28: // ( - charset designator
      case 0x29: // )
      case 0x2A: // *
      case 0x2B: // +
      case 0x23: // # - DEC alignment
        // The designator's argument is one byte; drop it without acting.
        _state = _State.charset;
      case 0x5C: // ST
      case 0x3D: // =
      case 0x3E: // >
      case 0x48: // H - tab set
      case 0x6E: // LS2
      case 0x6F: // LS3
        break;
    }
  }

  void _csi(int rune) {
    if (rune >= 0x40 && rune <= 0x7E) {
      _dispatchCsi(rune);
      _state = _State.ground;
      return;
    }
    if (rune >= 0x20 && rune <= 0x3F) {
      _csiBody.writeCharCode(rune);
      return;
    }
    // A control character interrupts and cancels the sequence.
    _state = _State.ground;
  }

  void _dispatchCsi(int finalByte) {
    final String body = _csiBody.toString();
    _csiBody.clear();
    bool private = false;
    int index = 0;
    if (body.isNotEmpty && body.codeUnitAt(0) == 0x3F) {
      private = true;
      index = 1;
    }
    final List<int?> params = <int?>[];
    int value = -1;
    for (; index < body.length; index++) {
      final int code = body.codeUnitAt(index);
      if (code == 0x3B) {
        params.add(value < 0 ? null : value);
        value = -1;
      } else if (code >= 0x30 && code <= 0x39) {
        value = (value < 0 ? 0 : value) * 10 + (code - 0x30);
        if (value > 999999) {
          value = 999999;
        }
      } else if (code >= 0x20 && code <= 0x2F) {
        // Intermediates (for example the space in DECSCUSR) do not change
        // any sequence this model acts on.
      } else {
        return;
      }
    }
    if (body.isNotEmpty) {
      params.add(value < 0 ? null : value);
    }
    switch (finalByte) {
      case 0x40: // @ insert characters
        _insertChars(_count(params, 0));
      case 0x41: // A
        _moveCursorRow(-_move(params, 0));
      case 0x42: // B
        _moveCursorRow(_move(params, 0));
      case 0x43: // C
        _moveCursorColumn(_move(params, 0));
      case 0x44: // D
        _moveCursorColumn(-_move(params, 0));
      case 0x45: // E
        _moveCursorRow(_move(params, 0));
        cursorColumn = 0;
      case 0x46: // F
        _moveCursorRow(-_move(params, 0));
        cursorColumn = 0;
      case 0x47: // G
        cursorColumn = (_count(params, 0, fallback: 1) - 1).clamp(0, _columns - 1);
        _wrapPending = false;
      case 0x48: // H
      case 0x66: // f
        _setCursor(_count(params, 0, fallback: 1) - 1,
            _count(params, 1, fallback: 1) - 1);
      case 0x49: // I
        _tab(_move(params, 0));
      case 0x4A: // J
        _eraseDisplay(_count(params, 0));
      case 0x4B: // K
        _eraseLine(_count(params, 0));
      case 0x4C: // L
        _insertLines(_move(params, 0));
      case 0x4D: // M
        _deleteLines(_move(params, 0));
      case 0x50: // P
        _deleteChars(_move(params, 0));
      case 0x53: // S
        _scrollUp(_move(params, 0), region: true);
      case 0x54: // T
        _scrollDown(_move(params, 0), region: true);
      case 0x58: // X
        _eraseChars(_move(params, 0));
      case 0x5A: // Z
        _backTab(_move(params, 0));
      case 0x64: // d
        _moveCursorRowTo(_count(params, 0, fallback: 1) - 1);
      case 0x68: // h
        _setMode(params, private, true);
      case 0x6C: // l
        _setMode(params, private, false);
      case 0x6D: // m
        _sgr(params);
      case 0x6E: // n
        _report(params, private);
      case 0x72: // r
        _setScrollRegion(params);
      case 0x73: // s
        _saveCursor();
      case 0x75: // u
        _restoreCursor();
    }
  }

  static int _count(List<int?> params, int index, {int fallback = 0}) {
    if (index >= params.length) {
      return fallback;
    }
    return params[index] ?? fallback;
  }

  /// A cursor-move argument; an explicit 0 means 1, as in xterm.
  static int _move(List<int?> params, int index) {
    final int value = _count(params, index, fallback: 1);
    return value <= 0 ? 1 : value;
  }

  int get _sidewaysLimit => _columns - 1;

  void _moveCursorRow(int delta) {
    final int low = cursorRow >= scrollTop && cursorRow <= scrollBottom
        ? scrollTop
        : 0;
    final int high = cursorRow >= scrollTop && cursorRow <= scrollBottom
        ? scrollBottom
        : _rows - 1;
    cursorRow = (cursorRow + delta).clamp(low, high);
    _wrapPending = false;
  }

  void _moveCursorRowTo(int row) {
    cursorRow = row.clamp(0, _rows - 1);
    _wrapPending = false;
  }

  void _moveCursorColumn(int delta) {
    final int column = (cursorColumn + delta).clamp(0, _sidewaysLimit);
    // Land on a wide character's leader instead of its continuation.
    cursorColumn = column > 0 && _lines[cursorRow][column].continuation
        ? column - 1
        : column;
    _wrapPending = false;
  }

  void _setCursor(int row, int column) {
    cursorRow = row.clamp(0, _rows - 1);
    cursorColumn = column.clamp(0, _sidewaysLimit);
    _wrapPending = false;
  }

  void _backTab(int count) {
    _wrapPending = false;
    for (int i = 0; i < count; i++) {
      int next = ((cursorColumn - 1) ~/ 8) * 8;
      if (next < 0) {
        next = 0;
      }
      cursorColumn = next;
    }
  }

  void _lineFeed() {
    if (cursorRow == scrollBottom) {
      _scrollUp(1, region: true);
    } else if (cursorRow < _rows - 1) {
      cursorRow++;
    }
  }

  void _reverseIndex() {
    if (cursorRow == scrollTop) {
      _scrollDown(1, region: true);
    } else if (cursorRow > 0) {
      cursorRow--;
    }
  }

  void _scrollUp(int count, {required bool region}) {
    final int top = region ? scrollTop : 0;
    final int bottom = region ? scrollBottom : _rows - 1;
    if (top >= bottom) {
      return;
    }
    final bool whole = top == 0 && bottom == _rows - 1;
    for (int i = 0; i < count; i++) {
      final List<TerminalCell> removed = _lines.removeAt(top);
      if (whole && !_altActive) {
        _pushScrollback(removed);
      }
      _lines.insert(bottom, _blankLine());
    }
  }

  void _scrollDown(int count, {required bool region}) {
    final int top = region ? scrollTop : 0;
    final int bottom = region ? scrollBottom : _rows - 1;
    if (top >= bottom) {
      return;
    }
    for (int i = 0; i < count; i++) {
      _lines.removeAt(bottom);
      _lines.insert(top, _blankLine());
    }
  }

  void _pushScrollback(List<TerminalCell> line) {
    if (maxScrollback <= 0) {
      return;
    }
    _scrollback.add(line);
    if (_scrollback.length > maxScrollback) {
      _scrollback.removeRange(0, _scrollback.length - maxScrollback);
    }
  }

  void _eraseDisplay(int mode) {
    switch (mode) {
      case 0:
        _eraseRange(cursorRow, cursorColumn, _columns - 1);
        for (int row = cursorRow + 1; row < _rows; row++) {
          _eraseRange(row, 0, _columns - 1);
        }
      case 1:
        for (int row = 0; row < cursorRow; row++) {
          _eraseRange(row, 0, _columns - 1);
        }
        _eraseRange(cursorRow, 0, cursorColumn);
      case 2:
        for (int row = 0; row < _rows; row++) {
          _eraseRange(row, 0, _columns - 1);
        }
      case 3:
        _scrollback.clear();
        for (int row = 0; row < _rows; row++) {
          _eraseRange(row, 0, _columns - 1);
        }
    }
  }

  void _eraseLine(int mode) {
    switch (mode) {
      case 0:
        _eraseRange(cursorRow, cursorColumn, _columns - 1);
      case 1:
        _eraseRange(cursorRow, 0, cursorColumn);
      case 2:
        _eraseRange(cursorRow, 0, _columns - 1);
    }
  }

  void _eraseRange(int row, int first, int last) {
    if (row < 0 || row >= _rows) {
      return;
    }
    final int from = first.clamp(0, _columns - 1);
    final int to = last.clamp(0, _columns - 1);
    if (from > to) {
      return;
    }
    final List<TerminalCell> line = _lines[row];
    for (int column = from; column <= to; column++) {
      _clearWideAt(line, column);
      line[column].clearTo(kTerminalDefaultColor, bg);
    }
  }

  void _insertChars(int count) {
    final List<TerminalCell> line = _lines[cursorRow];
    for (int i = 0; i < count; i++) {
      line.insert(cursorColumn, TerminalCell());
      line.removeLast();
    }
  }

  void _deleteChars(int count) {
    final List<TerminalCell> line = _lines[cursorRow];
    for (int i = 0; i < count; i++) {
      if (cursorColumn < line.length) {
        line.removeAt(cursorColumn);
        line.add(TerminalCell());
      }
    }
  }

  void _eraseChars(int count) {
    final int last = (cursorColumn + count - 1).clamp(0, _columns - 1);
    _eraseRange(cursorRow, cursorColumn, last);
  }

  void _insertLines(int count) {
    if (cursorRow < scrollTop || cursorRow > scrollBottom) {
      return;
    }
    for (int i = 0; i < count; i++) {
      _lines.removeAt(scrollBottom);
      _lines.insert(cursorRow, _blankLine());
    }
  }

  void _deleteLines(int count) {
    if (cursorRow < scrollTop || cursorRow > scrollBottom) {
      return;
    }
    for (int i = 0; i < count; i++) {
      _lines.removeAt(cursorRow);
      _lines.insert(scrollBottom, _blankLine());
    }
  }

  void _setScrollRegion(List<int?> params) {
    final int top = _count(params, 0, fallback: 1) - 1;
    final int bottom = _count(params, 1, fallback: _rows) - 1;
    if (top < 0 || bottom >= _rows || top >= bottom) {
      return;
    }
    scrollTop = top;
    scrollBottom = bottom;
    cursorRow = 0;
    cursorColumn = 0;
    _wrapPending = false;
  }

  void _setMode(List<int?> params, bool private, bool enable) {
    if (!private) {
      return;
    }
    for (final int? param in params) {
      switch (param) {
        case 1:
          appCursorKeys = enable;
        case 7:
          autoWrap = enable;
        case 25:
          cursorVisible = enable;
        case 47:
        case 1047:
        case 1049:
          _switchAlt(enable);
        case 2004:
          bracketedPaste = enable;
      }
    }
  }

  void _switchAlt(bool enter) {
    if (enter == _altActive) {
      return;
    }
    if (enter) {
      _savedScreen = _lines;
      _savedRow = cursorRow;
      _savedColumn = cursorColumn;
      _savedFg = fg;
      _savedBg = bg;
      _savedAttrs = attrs;
      _savedWrap = autoWrap;
      _savedScrollTop = scrollTop;
      _savedScrollBottom = scrollBottom;
      _lines = _blankLines(_rows, _columns);
      cursorRow = 0;
      cursorColumn = 0;
      scrollTop = 0;
      scrollBottom = _rows - 1;
      _altActive = true;
    } else {
      _lines = _savedScreen ?? _blankLines(_rows, _columns);
      _savedScreen = null;
      cursorRow = _savedRow.clamp(0, _rows - 1);
      cursorColumn = _savedColumn.clamp(0, _columns - 1);
      fg = _savedFg;
      bg = _savedBg;
      attrs = _savedAttrs;
      autoWrap = _savedWrap;
      if (_rows >= 2) {
        scrollTop = _savedScrollTop.clamp(0, _rows - 2);
        scrollBottom = _savedScrollBottom.clamp(scrollTop + 1, _rows - 1);
      } else {
        scrollTop = 0;
        scrollBottom = 0;
      }
      _altActive = false;
    }
    _wrapPending = false;
  }

  void _report(List<int?> params, bool private) {
    if (private) {
      return;
    }
    final int mode = _count(params, 0);
    if (mode == 5) {
      onReply?.call('\x1b[0n');
    } else if (mode == 6) {
      onReply?.call('\x1b[${cursorRow + 1};${cursorColumn + 1}R');
    }
  }

  void _saveCursor() {
    _savedRow = cursorRow;
    _savedColumn = cursorColumn;
    _savedFg = fg;
    _savedBg = bg;
    _savedAttrs = attrs;
    _savedWrap = autoWrap;
  }

  void _restoreCursor() {
    cursorRow = _savedRow.clamp(0, _rows - 1);
    cursorColumn = _savedColumn.clamp(0, _columns - 1);
    fg = _savedFg;
    bg = _savedBg;
    attrs = _savedAttrs;
    autoWrap = _savedWrap;
    _wrapPending = false;
  }

  void _sgr(List<int?> params) {
    if (params.isEmpty) {
      _resetSgr();
      return;
    }
    int index = 0;
    while (index < params.length) {
      final int code = params[index] ?? 0;
      index++;
      switch (code) {
        case 0:
          _resetSgr();
        case 1:
          attrs |= TerminalAttr.bold;
        case 2:
          attrs |= TerminalAttr.dim;
        case 3:
          attrs |= TerminalAttr.italic;
        case 4:
          attrs |= TerminalAttr.underline;
        case 7:
          attrs |= TerminalAttr.inverse;
        case 8:
          attrs |= TerminalAttr.hidden;
        case 22:
          attrs &= ~(TerminalAttr.bold | TerminalAttr.dim);
        case 23:
          attrs &= ~TerminalAttr.italic;
        case 24:
          attrs &= ~TerminalAttr.underline;
        case 27:
          attrs &= ~TerminalAttr.inverse;
        case 28:
          attrs &= ~TerminalAttr.hidden;
        case 39:
          fg = kTerminalDefaultColor;
        case 49:
          bg = kTerminalDefaultColor;
        default:
          if (code >= 30 && code <= 37) {
            fg = code - 30;
          } else if (code >= 40 && code <= 47) {
            bg = code - 40;
          } else if (code >= 90 && code <= 97) {
            fg = code - 90 + 8;
          } else if (code >= 100 && code <= 107) {
            bg = code - 100 + 8;
          } else if (code == 38 || code == 48) {
            final (int, int)? extended = _extendedColor(params, index);
            if (extended != null) {
              if (code == 38) {
                fg = extended.$1;
              } else {
                bg = extended.$1;
              }
              index += extended.$2;
            }
          }
      }
    }
  }

  /// Parses `5;n` and `2;r;g;b` after an SGR 38/48, returning the colour and
  /// how many parameters it consumed.
  (int, int)? _extendedColor(List<int?> params, int index) {
    if (index >= params.length) {
      return null;
    }
    final int kind = params[index] ?? 0;
    if (kind == 5 && index + 1 < params.length) {
      return ((params[index + 1] ?? 0).clamp(0, 255), 2);
    }
    if (kind == 2 && index + 3 < params.length) {
      final int red = (params[index + 1] ?? 0).clamp(0, 255);
      final int green = (params[index + 2] ?? 0).clamp(0, 255);
      final int blue = (params[index + 3] ?? 0).clamp(0, 255);
      return (kTerminalRgbFlag | (red << 16) | (green << 8) | blue, 4);
    }
    return null;
  }

  void _resetSgr() {
    fg = kTerminalDefaultColor;
    bg = kTerminalDefaultColor;
    attrs = 0;
  }

  void _osc(int rune) {
    if (rune == 0x07) {
      _commitOsc();
      _state = _State.ground;
      return;
    }
    if (rune == 0x1B) {
      _state = _State.oscEscape;
      return;
    }
    if (_oscBody.length < 512) {
      _oscBody.writeCharCode(rune);
    }
  }

  void _commitOsc() {
    final String body = _oscBody.toString();
    _oscBody.clear();
    final int separator = body.indexOf(';');
    if (separator <= 0) {
      return;
    }
    final String command = body.substring(0, separator);
    if (command == '0' || command == '2') {
      title = body.substring(separator + 1);
    }
  }

  // --------------------------------------------------------------- helpers

  List<TerminalCell> _blankLine() {
    return List<TerminalCell>.generate(
        _columns, (_) => TerminalCell(), growable: true);
  }

  List<List<TerminalCell>> _blankLines(int rows, int columns) {
    return List<List<TerminalCell>>.generate(rows, (_) {
      return List<TerminalCell>.generate(
          columns, (_) => TerminalCell(), growable: true);
    });
  }

  void _resizeLine(List<TerminalCell> line) {
    if (line.length > _columns) {
      line.removeRange(_columns, line.length);
      if (line.isNotEmpty && line.last.continuation) {
        line.last.clearTo(kTerminalDefaultColor, bg);
      }
    } else {
      while (line.length < _columns) {
        line.add(TerminalCell());
      }
    }
  }
}
