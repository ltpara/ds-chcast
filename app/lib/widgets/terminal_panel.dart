import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../terminal/terminal_keys.dart';
import '../terminal/terminal_model.dart';
import '../theme.dart';
import '../viewer_bridge.dart';

const double _kFontSize = 13;
const double _kLineHeight = 16;
const double _kBodyPadding = 8;

/// The xterm 16-colour palette, matching the dark console look.
const List<Color> _kPalette = <Color>[
  Color(0xFF1E1E1E),
  Color(0xFFCD3131),
  Color(0xFF0DBC79),
  Color(0xFFE5E510),
  Color(0xFF2472C8),
  Color(0xFFBC3FBC),
  Color(0xFF11A8CD),
  Color(0xFFE5E5E5),
  Color(0xFF666666),
  Color(0xFFF14C4C),
  Color(0xFF23D18B),
  Color(0xFFF5F543),
  Color(0xFF3B8EEA),
  Color(0xFFD670D6),
  Color(0xFF29B8DB),
  Color(0xFFFFFFFF),
];

const Color _kForeground = Color(0xFFD4D4D4);
const Color _kBackground = Color(0xFF0C0C0C);

/// Modeless remote-console overlay. The page owns the transport callbacks and
/// the focus node; the panel owns the screen model, the UTF-8 reassembly and
/// the output poll timer.
class TerminalPanel extends StatefulWidget {
  const TerminalPanel({
    super.key,
    required this.status,
    required this.focusNode,
    required this.onOpen,
    required this.onTakeOutput,
    required this.onSendInput,
    required this.onResize,
    required this.onClose,
    required this.onDismiss,
  });

  final ViewerStatus status;
  final FocusNode focusNode;
  final Future<bool> Function(int columns, int rows) onOpen;
  final Future<ViewerTerminalOutput?> Function() onTakeOutput;
  final Future<bool> Function(String input) onSendInput;
  final Future<bool> Function(int columns, int rows) onResize;
  final Future<void> Function() onClose;
  final VoidCallback onDismiss;

  @override
  State<TerminalPanel> createState() => _TerminalPanelState();
}

class _TerminalPanelState extends State<TerminalPanel> {
  static final double _cellWidth = _measureCellWidth();

  final TerminalGrid _grid = TerminalGrid();
  final TerminalUtf8Decoder _decoder = TerminalUtf8Decoder();
  final ScrollController _scroll = ScrollController();

  Timer? _poll;
  Timer? _resizeDebounce;
  bool _opening = false;
  bool _openQueued = false;
  bool _openFailed = false;
  bool _draining = false;
  bool _ended = false;
  bool _follow = true;
  int _layoutColumns = 0;
  int _layoutRows = 0;
  int _sentColumns = 0;
  int _sentRows = 0;

  @override
  void initState() {
    super.initState();
    _grid.onReply = _send;
    _scroll.addListener(_onScroll);
  }

  @override
  void dispose() {
    _poll?.cancel();
    _resizeDebounce?.cancel();
    _scroll.removeListener(_onScroll);
    _scroll.dispose();
    unawaited(_closeQuietly());
    super.dispose();
  }

  static double _measureCellWidth() {
    final TextPainter painter = TextPainter(
      text: const TextSpan(
        text: 'MMMMMMMMMMMMMMMM',
        style: TextStyle(
          fontFamily: 'Consolas',
          fontFamilyFallback: <String>['Cascadia Mono', 'Courier New'],
          fontSize: _kFontSize,
          height: 1.0,
        ),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    final double width = painter.width / 16;
    painter.dispose();
    // A font-less environment (widget tests) still needs a usable cell.
    return width.isFinite && width > 0 ? width : 7.8;
  }

  void _onScroll() {
    if (!_scroll.hasClients) {
      return;
    }
    final bool atBottom =
        _scroll.position.pixels >= _scroll.position.maxScrollExtent - 4;
    if (atBottom != _follow) {
      setState(() => _follow = atBottom);
    }
  }

  // ------------------------------------------------------------ transport

  /// Applies the grid size the body layout allows, opens the console on the
  /// first pass, and tells the host about later window changes.
  void _applySize(int columns, int rows) {
    if (!mounted) {
      return;
    }
    final bool first = !_openQueued;
    if (!first && columns == _layoutColumns && rows == _layoutRows) {
      return;
    }
    _layoutColumns = columns;
    _layoutRows = rows;
    _grid.resize(columns, rows);
    if (first) {
      _openQueued = true;
      setState(() {});
      unawaited(_open(columns, rows));
      return;
    }
    setState(() {});
    if (_sentColumns == columns && _sentRows == rows) {
      return;
    }
    _resizeDebounce?.cancel();
    _resizeDebounce = Timer(const Duration(milliseconds: 120), () {
      if (!mounted) {
        return;
      }
      unawaited(_resize(columns, rows));
    });
  }

  Future<void> _open(int columns, int rows) async {
    if (_opening) {
      return;
    }
    _opening = true;
    bool queued = false;
    try {
      queued = await widget.onOpen(columns, rows);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    } finally {
      _opening = false;
    }
    if (!mounted) {
      return;
    }
    if (!queued) {
      setState(() => _openFailed = true);
      return;
    }
    _sentColumns = columns;
    _sentRows = rows;
    _startPolling();
  }

  Future<void> _resize(int columns, int rows) async {
    if (_ended || _openFailed) {
      return;
    }
    try {
      final bool queued = await widget.onResize(columns, rows);
      if (queued) {
        _sentColumns = columns;
        _sentRows = rows;
      }
    } on PlatformException {
      // The status poll reports a closing session.
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  void _startPolling() {
    _poll ??= Timer.periodic(const Duration(milliseconds: 30), (_) {
      unawaited(_drain());
    });
  }

  void _stopPolling() {
    _poll?.cancel();
    _poll = null;
  }

  Future<void> _drain() async {
    if (_draining || _ended) {
      return;
    }
    _draining = true;
    bool changed = false;
    try {
      while (mounted && !_ended) {
        final ViewerTerminalOutput? output = await widget.onTakeOutput();
        if (output == null) {
          break;
        }
        if (output.data.isNotEmpty) {
          final String text = _decoder.add(output.data);
          if (text.isNotEmpty) {
            _grid.write(text);
            changed = true;
          }
        }
        if (output.ended) {
          _ended = true;
        }
      }
    } on PlatformException {
      _stopPolling();
    } on MissingPluginException {
      _stopPolling();
    } finally {
      _draining = false;
    }
    if (!mounted) {
      return;
    }
    if (_ended) {
      _stopPolling();
    }
    if (changed || _ended) {
      setState(() {});
      if (changed && _follow) {
        _scheduleScrollToBottom();
      }
    }
  }

  Future<void> _closeQuietly() async {
    try {
      await widget.onClose();
    } on PlatformException {
      // A closing session already dropped the console.
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  void _send(String data) {
    if (data.isEmpty || !_canType) {
      return;
    }
    unawaited(_sendQuietly(data));
  }

  Future<void> _sendQuietly(String data) async {
    try {
      await widget.onSendInput(data);
    } on PlatformException {
      // The session is closing.
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  Future<void> _paste() async {
    final ClipboardData? data = await Clipboard.getData(Clipboard.kTextPlain);
    final String? text = data?.text;
    if (text == null || text.isEmpty || !mounted) {
      return;
    }
    final String normalised =
        text.replaceAll('\r\n', '\n').replaceAll('\n', '\r');
    if (_grid.bracketedPaste) {
      _send('\x1b[200~$normalised\x1b[201~');
    } else {
      _send(normalised);
    }
  }

  bool get _canType =>
      !_ended &&
      !_openFailed &&
      (widget.status.terminalPhase == ViewerTerminalPhase.open ||
          widget.status.terminalPhase == ViewerTerminalPhase.requesting);

  void _scheduleScrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _scroll.hasClients) {
        _scroll.jumpTo(_scroll.position.maxScrollExtent);
      }
    });
  }

  void _reopen() {
    _resizeDebounce?.cancel();
    _stopPolling();
    _decoder.reset();
    _grid.reset();
    _grid.resize(math.max(1, _layoutColumns), math.max(1, _layoutRows));
    _sentColumns = 0;
    _sentRows = 0;
    setState(() {
      _ended = false;
      _openFailed = false;
    });
    unawaited(_open(math.max(1, _layoutColumns), math.max(1, _layoutRows)));
  }

  // ------------------------------------------------------------------ keys

  KeyEventResult _onKey(FocusNode node, KeyEvent event) {
    if (event is KeyUpEvent) {
      return KeyEventResult.handled;
    }
    if (!_canType) {
      return KeyEventResult.handled;
    }
    final HardwareKeyboard keyboard = HardwareKeyboard.instance;
    final bool ctrl = keyboard.isControlPressed;
    final bool shift = keyboard.isShiftPressed;
    final LogicalKeyboardKey key = event.logicalKey;
    if ((key == LogicalKeyboardKey.keyV && ctrl) ||
        (key == LogicalKeyboardKey.insert && shift)) {
      unawaited(_paste());
      return KeyEventResult.handled;
    }
    final String? sequence = terminalKeySequence(event);
    if (sequence != null) {
      _send(sequence);
      return KeyEventResult.handled;
    }
    final String? character = event.character;
    if (character != null && character.isNotEmpty) {
      bool printable = true;
      for (final int rune in character.runes) {
        if (rune < 0x20 || rune == 0x7F) {
          printable = false;
          break;
        }
      }
      if (printable) {
        _send(keyboard.isAltPressed ? '\x1b$character' : character);
      }
    }
    return KeyEventResult.handled;
  }

  // ----------------------------------------------------------------- build

  @override
  Widget build(BuildContext context) {
    final bool busy = widget.status.terminalPhase == ViewerTerminalPhase.requesting;
    return Focus(
      focusNode: widget.focusNode,
      onKeyEvent: _onKey,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: () => widget.focusNode.requestFocus(),
        child: Material(
          color: _kBackground,
          elevation: 12,
          borderRadius: BorderRadius.circular(8),
          child: Container(
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(8),
              border: Border.all(color: Colors.white24),
            ),
            clipBehavior: Clip.antiAlias,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                _buildHeader(busy),
                const Divider(height: 1, thickness: 1, color: Colors.white12),
                Expanded(child: _buildBody()),
                const Divider(height: 1, thickness: 1, color: Colors.white12),
                _buildFooter(),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildHeader(bool busy) {
    final String title = _grid.title.isEmpty ? '远程终端' : _grid.title;
    return Container(
      height: 36,
      color: const Color(0xFF1B1C20),
      padding: const EdgeInsets.only(left: 10, right: 4),
      child: Row(
        children: <Widget>[
          const Icon(Icons.terminal_rounded, size: 16, color: kAccentBright),
          const SizedBox(width: 8),
          Expanded(
            child: Tooltip(
              message: title,
              waitDuration: const Duration(milliseconds: 500),
              child: Text(
                title,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(
                  color: Colors.white70,
                  fontSize: 12,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
          ),
          if (busy)
            const Padding(
              padding: EdgeInsets.only(left: 8, right: 6),
              child: SizedBox(
                width: 12,
                height: 12,
                child: CircularProgressIndicator(strokeWidth: 1.6),
              ),
            ),
          Text(
            _phaseLabel(),
            style: TextStyle(
              fontSize: 10.5,
              color: busy ? Colors.white54 : Colors.white38,
            ),
          ),
          const SizedBox(width: 10),
          Text(
            '${_sentColumns > 0 ? _sentColumns : _grid.columns}×'
            '${_sentRows > 0 ? _sentRows : _grid.rows}',
            style: const TextStyle(fontSize: 10.5, color: Colors.white38),
          ),
          const SizedBox(width: 2),
          if (_ended || _openFailed)
            _headerButton(
              icon: Icons.refresh_rounded,
              tooltip: '重新申请终端',
              onTap: _reopen,
            ),
          _headerButton(
            icon: Icons.content_paste_rounded,
            tooltip: '粘贴（Ctrl+V）',
            disabled: !_canType,
            onTap: () => unawaited(_paste()),
          ),
          _headerButton(
            icon: Icons.close_rounded,
            tooltip: '关闭终端（同时结束主机上的控制台）',
            onTap: widget.onDismiss,
          ),
        ],
      ),
    );
  }

  Widget _headerButton({
    required IconData icon,
    required String tooltip,
    required VoidCallback onTap,
    bool disabled = false,
  }) {
    return Tooltip(
      message: tooltip,
      waitDuration: const Duration(milliseconds: 450),
      child: SizedBox(
        width: 30,
        height: 30,
        child: IconButton(
          padding: EdgeInsets.zero,
          constraints: const BoxConstraints.tightFor(width: 30, height: 30),
          visualDensity: VisualDensity.compact,
          splashRadius: 15,
          iconSize: 16,
          onPressed: disabled ? null : onTap,
          icon: Icon(icon, color: disabled ? Colors.white24 : Colors.white54),
        ),
      ),
    );
  }

  String _phaseLabel() {
    switch (widget.status.terminalPhase) {
      case ViewerTerminalPhase.requesting:
        return '申请中…';
      case ViewerTerminalPhase.open:
        return _ended ? '已结束' : '已连接';
      case ViewerTerminalPhase.ended:
        return '已结束';
      case ViewerTerminalPhase.failed:
        return '失败';
      case ViewerTerminalPhase.closed:
        return _openFailed ? '未连接' : '准备中';
    }
  }

  Widget _buildBody() {
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final double width =
            math.max(0, constraints.maxWidth - _kBodyPadding * 2);
        final double height =
            math.max(0, constraints.maxHeight - _kBodyPadding * 2);
        final int columns =
            (width / _cellWidth).floor().clamp(1, 500);
        final int rows = (height / _kLineHeight).floor().clamp(1, 200);
        if ((columns != _layoutColumns || rows != _layoutRows) && columns > 0) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            _applySize(columns, rows);
          });
        }
        final String notice = _notice();
        final List<List<TerminalCell>> history = _grid.scrollback;
        final int historyLength = history.length;
        final int screenRows = _grid.rows;
        final bool cursorOnScreen = !_ended && _grid.cursorVisible;
        return Column(
          children: <Widget>[
            if (notice.isNotEmpty) _buildNotice(notice),
            Expanded(
              child: ListView.builder(
                controller: _scroll,
                padding: const EdgeInsets.symmetric(
                    horizontal: _kBodyPadding, vertical: _kBodyPadding),
                itemExtent: _kLineHeight,
                itemCount: historyLength + screenRows,
                itemBuilder: (BuildContext context, int index) {
                  final bool inHistory = index < historyLength;
                  final List<TerminalCell> cells = inHistory
                      ? history[index]
                      : _grid.lines[index - historyLength];
                  final int cursorColumn = !inHistory &&
                          cursorOnScreen &&
                          index - historyLength == _grid.cursorRow
                      ? _grid.cursorColumn
                      : -1;
                  return _TerminalLine(
                    cells: cells,
                    cellWidth: _cellWidth,
                    cursorColumn: cursorColumn,
                  );
                },
              ),
            ),
          ],
        );
      },
    );
  }

  String _notice() {
    if (_openFailed) {
      return '无法打开终端：主机未授权、已有控制台，或上一个控制台尚未收尾';
    }
    if (_ended) {
      final String reason = widget.status.terminalReason;
      return reason.isEmpty ? '主机已结束该控制台' : '主机已结束该控制台：$reason';
    }
    switch (widget.status.terminalPhase) {
      case ViewerTerminalPhase.requesting:
        return '';
      case ViewerTerminalPhase.ended:
        final String reason = widget.status.terminalReason;
        return reason.isEmpty ? '主机已结束该控制台' : '主机已结束该控制台：$reason';
      case ViewerTerminalPhase.failed:
        final String detail = widget.status.terminalError.isNotEmpty
            ? widget.status.terminalError
            : widget.status.terminalReason;
        return detail.isEmpty ? '终端不可用（主机拒绝或权限已撤销）' : detail;
      default:
        return '';
    }
  }

  Widget _buildNotice(String text) {
    return Container(
      width: double.infinity,
      color: const Color(0xFF3A2C12),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      child: Row(
        children: <Widget>[
          const Icon(Icons.info_outline_rounded,
              size: 13, color: Colors.amberAccent),
          const SizedBox(width: 6),
          Expanded(
            child: Text(
              text,
              style: const TextStyle(fontSize: 11, color: Colors.amberAccent),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildFooter() {
    return Container(
      height: 22,
      color: const Color(0xFF1B1C20),
      padding: const EdgeInsets.symmetric(horizontal: 10),
      child: Row(
        children: <Widget>[
          const Expanded(
            child: Text(
              '点击面板后直接输入 · Ctrl+V 粘贴 · Esc 交给主机',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(fontSize: 10, color: Colors.white38),
            ),
          ),
          Text(
            '${widget.status.terminalBytes} 字节',
            style: const TextStyle(fontSize: 10, color: Colors.white24),
          ),
        ],
      ),
    );
  }
}

/// One rendered row: style runs from the grid plus an optional cursor block.
class _TerminalLine extends StatelessWidget {
  const _TerminalLine({
    required this.cells,
    required this.cellWidth,
    required this.cursorColumn,
  });

  final List<TerminalCell> cells;
  final double cellWidth;
  final int cursorColumn;

  @override
  Widget build(BuildContext context) {
    final List<InlineSpan> spans = <InlineSpan>[];
    StringBuffer text = StringBuffer();
    int? runFg;
    int? runBg;
    int runAttrs = 0;
    void flush() {
      if (text.isEmpty) {
        return;
      }
      spans.add(TextSpan(
        text: text.toString(),
        style: _styleFor(runFg ?? kTerminalDefaultColor,
            runBg ?? kTerminalDefaultColor, runAttrs),
      ));
      text = StringBuffer();
    }

    for (final TerminalCell cell in cells) {
      if (cell.continuation) {
        continue;
      }
      if (runFg != null && (cell.fg != runFg || cell.bg != runBg || cell.attrs != runAttrs)) {
        flush();
      }
      runFg = cell.fg;
      runBg = cell.bg;
      runAttrs = cell.attrs;
      text.write(cell.text);
    }
    flush();

    return SizedBox(
      height: _kLineHeight,
      child: Stack(
        children: <Widget>[
          RichText(
            text: TextSpan(
              style: _baseStyle,
              children: spans,
            ),
            maxLines: 1,
            softWrap: false,
            overflow: TextOverflow.clip,
          ),
          if (cursorColumn >= 0)
            Positioned(
              left: cursorColumn * cellWidth,
              top: 0,
              width: cellWidth,
              height: _kLineHeight,
              child: IgnorePointer(
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: kAccentBright.withValues(alpha: 0.42),
                    border: Border.all(
                        color: kAccentBright.withValues(alpha: 0.8),
                        width: 0.8),
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }

  static const TextStyle _baseStyle = TextStyle(
    fontFamily: 'Consolas',
    fontFamilyFallback: <String>['Cascadia Mono', 'Courier New'],
    fontSize: _kFontSize,
    height: 1.0,
    color: _kForeground,
  );

  static TextStyle _styleFor(int fg, int bg, int attrs) {
    bool inverse = (attrs & TerminalAttr.inverse) != 0;
    Color foreground = _colorFor(fg, _kForeground);
    Color background = _colorFor(bg, _kBackground);
    if (inverse) {
      final Color swap = foreground;
      foreground = background;
      background = swap;
    }
    if ((attrs & TerminalAttr.dim) != 0) {
      foreground = foreground.withValues(alpha: 0.6);
    }
    if ((attrs & TerminalAttr.hidden) != 0) {
      foreground = background;
    }
    return TextStyle(
      fontFamily: 'Consolas',
      fontFamilyFallback: const <String>['Cascadia Mono', 'Courier New'],
      fontSize: _kFontSize,
      height: 1.0,
      color: foreground,
      backgroundColor: background == _kBackground ? null : background,
      fontWeight:
          (attrs & TerminalAttr.bold) != 0 ? FontWeight.w700 : FontWeight.w400,
      fontStyle:
          (attrs & TerminalAttr.italic) != 0 ? FontStyle.italic : FontStyle.normal,
      decoration: (attrs & TerminalAttr.underline) != 0
          ? TextDecoration.underline
          : TextDecoration.none,
      decorationColor: foreground,
    );
  }

  static Color _colorFor(int value, Color fallback) {
    if (value == kTerminalDefaultColor) {
      return fallback;
    }
    if ((value & kTerminalRgbFlag) != 0) {
      return Color(0xFF000000 | (value & 0xFFFFFF));
    }
    if (value < 16) {
      return _kPalette[value];
    }
    if (value < 232) {
      // 6x6x6 cube: 16 + 36r + 6g + b with 0/95/135/175/215/255 levels.
      final int index = value - 16;
      final int red = index ~/ 36;
      final int green = (index ~/ 6) % 6;
      final int blue = index % 6;
      int level(int part) => part == 0 ? 0 : 55 + part * 40;
      return Color.fromARGB(255, level(red), level(green), level(blue));
    }
    // 232..255 is a 24-step grey ramp.
    final int grey = 8 + (value - 232) * 10;
    return Color.fromARGB(255, grey, grey, grey);
  }
}
