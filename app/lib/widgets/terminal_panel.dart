import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../terminal/terminal_keys.dart';
import '../terminal/terminal_model.dart';
import '../theme.dart';
import '../viewer_bridge.dart';
import 'package:chast_viewer/l10n.dart';

const double _kFontSize = 13;
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
    this.terminalId = 1,
    required this.focusNode,
    required this.onOpen,
    required this.onTakeOutput,
    required this.onSendInput,
    required this.onResize,
    required this.onClose,
    required this.onDismiss,
    this.initialFontSize = 13,
    this.onFontSizeChanged,
  });

  final ViewerStatus status;
  final int terminalId;
  final FocusNode focusNode;
  final Future<bool> Function(int columns, int rows) onOpen;
  final Future<ViewerTerminalOutput?> Function() onTakeOutput;
  final Future<bool> Function(String input) onSendInput;
  final Future<bool> Function(int columns, int rows) onResize;
  final Future<void> Function() onClose;
  final VoidCallback onDismiss;
  final int initialFontSize;
  final ValueChanged<int>? onFontSizeChanged;

  @override
  State<TerminalPanel> createState() => _TerminalPanelState();
}

class _TerminalPanelState extends State<TerminalPanel> {
  static const double _minFontSize = 10;
  static const double _maxFontSize = 20;
  static const double _defaultFontSize = 13;

  double _fontSize = _kFontSize;
  late double _cellWidth = _measureCellWidth(_fontSize);
  double get _lineHeight => (_fontSize * 1.23).roundToDouble();

  final TerminalGrid _grid = TerminalGrid();
  final TerminalUtf8Decoder _decoder = TerminalUtf8Decoder();
  final ScrollController _scroll = ScrollController();
  final TextEditingController _search = TextEditingController();
  final FocusNode _searchFocus = FocusNode();

  ViewerTerminalStatus get _terminal => widget.status.terminal(widget.terminalId);

  Timer? _poll;
  Timer? _resizeDebounce;
  bool _opening = false;
  bool _openQueued = false;
  bool _openFailed = false;
  bool _draining = false;
  bool _ended = false;
  bool _follow = true;
  bool _searching = false;
  String _searchQuery = '';
  List<int> _matches = <int>[];
  int _matchIndex = 0;
  int _layoutColumns = 0;
  int _layoutRows = 0;
  int _sentColumns = 0;
  int _sentRows = 0;

  @override
  void initState() {
    super.initState();
    _fontSize = widget.initialFontSize.toDouble().clamp(
      _minFontSize,
      _maxFontSize,
    );
    _grid.onReply = _send;
    _scroll.addListener(_onScroll);
  }

  @override
  void dispose() {
    _poll?.cancel();
    _resizeDebounce?.cancel();
    _scroll.removeListener(_onScroll);
    _scroll.dispose();
    _search.dispose();
    _searchFocus.dispose();
    unawaited(_closeQuietly());
    super.dispose();
  }

  static double _measureCellWidth(double fontSize) {
    final TextPainter painter = TextPainter(
      text: TextSpan(
        text: 'MMMMMMMMMMMMMMMM',
        style: TextStyle(
          fontFamily: 'Consolas',
          fontFamilyFallback: const <String>['Cascadia Mono', 'Courier New'],
          fontSize: fontSize,
          height: 1.0,
        ),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    final double width = painter.width / 16;
    painter.dispose();
    // A font-less environment (widget tests) still needs a usable cell.
    return width.isFinite && width > 0 ? width : 7.8 * fontSize / _kFontSize;
  }

  void _setFontSize(double delta) {
    _applyFontSize(_fontSize + delta);
  }

  void _resetFontSize() {
    _applyFontSize(_defaultFontSize);
  }

  void _applyFontSize(double next) {
    final double clamped = next.clamp(_minFontSize, _maxFontSize);
    if (clamped == _fontSize) {
      return;
    }
    setState(() {
      _fontSize = clamped;
      _cellWidth = _measureCellWidth(clamped);
      // Force the layout builder to re-measure and tell the host the new size.
      _layoutColumns = 0;
      _layoutRows = 0;
    });
    _follow = true;
    _scheduleScrollToBottom();
    widget.onFontSizeChanged?.call(clamped.round());
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
    final String normalised = text
        .replaceAll('\r\n', '\n')
        .replaceAll('\n', '\r');
    if (_grid.bracketedPaste) {
      _send('\x1b[200~$normalised\x1b[201~');
    } else {
      _send(normalised);
    }
  }

  // The grid pads every row to the terminal width, so trailing blanks are
  // dropped per row. Scrollback is included: users copy the console to paste
  // logs elsewhere, and the visible screen alone would lose the beginning.
  String _consoleContents() {
    final List<List<TerminalCell>> rows = <List<TerminalCell>>[
      ..._grid.scrollback,
      ..._grid.lines,
    ];
    final StringBuffer out = StringBuffer();
    for (final List<TerminalCell> row in rows) {
      final StringBuffer line = StringBuffer();
      for (final TerminalCell cell in row) {
        if (!cell.continuation) {
          line.write(cell.text);
        }
      }
      out.write(line.toString().trimRight());
      out.write('\n');
    }
    return out.toString().trimRight();
  }

  Future<void> _copyAll() async {
    final String text = _consoleContents();
    if (text.isEmpty) {
      _showNotice(tr('终端没有可复制的内容'));
      return;
    }
    bool copied = true;
    try {
      await Clipboard.setData(ClipboardData(text: text));
    } catch (_) {
      copied = false;
    }
    if (!mounted) {
      return;
    }
    _showNotice(copied ? tr('已复制终端内容') : tr('复制失败'));
  }

  void _showNotice(String message) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(message), duration: const Duration(seconds: 2)),
    );
  }

  bool get _canType =>
      !_ended &&
      !_openFailed &&
      (_terminal.phase == ViewerTerminalPhase.open ||
          _terminal.phase == ViewerTerminalPhase.requesting);

  void _scheduleScrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _scroll.hasClients) {
        _scroll.jumpTo(_scroll.position.maxScrollExtent);
      }
    });
  }

  void _scrollToBottom() {
    setState(() => _follow = true);
    if (_scroll.hasClients) {
      _scroll.jumpTo(_scroll.position.maxScrollExtent);
    }
  }

  void _scrollPage(int direction) {
    if (!_scroll.hasClients) {
      return;
    }
    final ScrollPosition position = _scroll.position;
    final double page = math.max(
      _lineHeight,
      position.viewportDimension - _lineHeight,
    );
    final double target = (position.pixels + direction * page).clamp(
      0.0,
      position.maxScrollExtent,
    );
    _scroll.jumpTo(target);
    setState(() => _follow = target >= position.maxScrollExtent - 4);
  }

  void _scrollToEdge({required bool top}) {
    if (!_scroll.hasClients) {
      return;
    }
    final ScrollPosition position = _scroll.position;
    final double target = top ? 0 : position.maxScrollExtent;
    _scroll.jumpTo(target);
    setState(() => _follow = target >= position.maxScrollExtent - 4);
  }

  // --------------------------------------------------------------- search

  void _openSearch() {
    setState(() => _searching = true);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) {
        _searchFocus.requestFocus();
      }
    });
  }

  void _closeSearch() {
    _search.clear();
    setState(() {
      _searching = false;
      _searchQuery = '';
      _matches = <int>[];
      _matchIndex = 0;
    });
    widget.focusNode.requestFocus();
  }

  static String _rowText(List<TerminalCell> row) {
    final StringBuffer out = StringBuffer();
    for (final TerminalCell cell in row) {
      if (!cell.continuation) {
        out.write(cell.text);
      }
    }
    return out.toString().trimRight();
  }

  void _updateSearch(String value) {
    final String query = value.trim().toLowerCase();
    final List<int> matches = <int>[];
    if (query.isNotEmpty) {
      final List<List<TerminalCell>> rows = <List<TerminalCell>>[
        ..._grid.scrollback,
        ..._grid.lines,
      ];
      for (int i = 0; i < rows.length; i++) {
        if (_rowText(rows[i]).toLowerCase().contains(query)) {
          matches.add(i);
        }
      }
    }
    setState(() {
      _searchQuery = query;
      _matches = matches;
      _matchIndex = matches.isEmpty ? 0 : matches.length - 1;
    });
    if (matches.isNotEmpty) {
      _jumpToMatch(matches.length - 1);
    }
  }

  void _stepMatch(int delta) {
    if (_matches.isEmpty) {
      return;
    }
    _jumpToMatch((_matchIndex + delta) % _matches.length);
  }

  void _jumpToMatch(int index) {
    setState(() => _matchIndex = index);
    final int row = _matches[index];
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_scroll.hasClients) {
        return;
      }
      final double max = _scroll.position.maxScrollExtent;
      final double target = (_kBodyPadding + row * _lineHeight).clamp(0.0, max);
      _scroll.jumpTo(target);
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
    final HardwareKeyboard keyboard = HardwareKeyboard.instance;
    final bool ctrl = keyboard.isControlPressed;
    final bool shift = keyboard.isShiftPressed;
    final LogicalKeyboardKey key = event.logicalKey;
    if (_searching) {
      if (key == LogicalKeyboardKey.escape) {
        _closeSearch();
      }
      return KeyEventResult.handled;
    }
    if (shift &&
        (key == LogicalKeyboardKey.pageUp ||
            key == LogicalKeyboardKey.pageDown ||
            key == LogicalKeyboardKey.home ||
            key == LogicalKeyboardKey.end)) {
      if (key == LogicalKeyboardKey.home) {
        _scrollToEdge(top: true);
      } else if (key == LogicalKeyboardKey.end) {
        _scrollToEdge(top: false);
      } else {
        _scrollPage(key == LogicalKeyboardKey.pageUp ? -1 : 1);
      }
      return KeyEventResult.handled;
    }
    if (!_canType) {
      return KeyEventResult.handled;
    }
    if (key == LogicalKeyboardKey.keyF && ctrl && !shift) {
      _openSearch();
      return KeyEventResult.handled;
    }
    if (key == LogicalKeyboardKey.keyC && ctrl && shift) {
      unawaited(_copyAll());
      return KeyEventResult.handled;
    }
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
    final bool busy =
        _terminal.phase == ViewerTerminalPhase.requesting;
    return Focus(
      focusNode: widget.focusNode,
      onKeyEvent: _onKey,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: () {
          if (!_searching) {
            widget.focusNode.requestFocus();
          }
        },
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
                if (_searching) _buildSearchBar(),
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
    final String title = _grid.title.isEmpty ? tr('远程终端') : _grid.title;
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
              tooltip: tr('重新申请终端'),
              onTap: _reopen,
            ),
          _headerButton(
            icon: Icons.text_decrease_rounded,
            tooltip: tr('缩小字号'),
            disabled: _fontSize <= _minFontSize,
            onTap: () => _setFontSize(-1),
          ),
          _headerButton(
            icon: Icons.text_increase_rounded,
            tooltip: tr('放大字号'),
            disabled: _fontSize >= _maxFontSize,
            onTap: () => _setFontSize(1),
          ),
          _headerButton(
            icon: Icons.restart_alt_rounded,
            tooltip: tr('重置字号'),
            disabled: _fontSize == _defaultFontSize,
            onTap: _resetFontSize,
          ),
          _headerButton(
            icon: Icons.search_rounded,
            tooltip: tr('搜索终端内容（Ctrl+F）'),
            onTap: _openSearch,
          ),
          _headerButton(
            icon: Icons.copy_rounded,
            tooltip: tr('复制终端内容（Ctrl+Shift+C，含滚动历史）'),
            onTap: () => unawaited(_copyAll()),
          ),
          _headerButton(
            icon: Icons.content_paste_rounded,
            tooltip: tr('粘贴（Ctrl+V）'),
            disabled: !_canType,
            onTap: () => unawaited(_paste()),
          ),
          _headerButton(
            icon: Icons.close_rounded,
            tooltip: tr('关闭终端（同时结束主机上的控制台）'),
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
    switch (_terminal.phase) {
      case ViewerTerminalPhase.requesting:
        return tr('申请中…');
      case ViewerTerminalPhase.open:
        return _ended ? tr('已结束') : tr('已连接');
      case ViewerTerminalPhase.ended:
        return tr('已结束');
      case ViewerTerminalPhase.failed:
        return tr('失败');
      case ViewerTerminalPhase.closed:
        return _openFailed ? tr('未连接') : tr('准备中');
    }
  }

  Widget _buildSearchBar() {
    final String count = _searchQuery.isEmpty
        ? ''
        : (_matches.isEmpty
              ? tr('无匹配')
              : '${_matchIndex + 1}/${_matches.length}');
    return Container(
      height: 36,
      color: const Color(0xFF1B1C20),
      padding: const EdgeInsets.only(left: 10, right: 4),
      child: Row(
        children: <Widget>[
          const Icon(Icons.search_rounded, size: 15, color: Colors.white38),
          const SizedBox(width: 6),
          Expanded(
            child: TextField(
              key: const Key('terminal-search-field'),
              controller: _search,
              focusNode: _searchFocus,
              style: const TextStyle(fontSize: 12, color: _kForeground),
              cursorColor: kAccentBright,
              decoration: InputDecoration(
                isDense: true,
                border: InputBorder.none,
                hintText: tr('输入要查找的文本（历史与当前屏幕）'),
                hintStyle: const TextStyle(
                  fontSize: 11.5,
                  color: Colors.white30,
                ),
              ),
              onChanged: _updateSearch,
              onSubmitted: (_) => _stepMatch(-1),
            ),
          ),
          Text(
            count,
            key: const Key('terminal-search-count'),
            style: const TextStyle(fontSize: 10.5, color: Colors.white54),
          ),
          _headerButton(
            icon: Icons.keyboard_arrow_up_rounded,
            tooltip: tr('上一个匹配'),
            disabled: _matches.isEmpty,
            onTap: () => _stepMatch(-1),
          ),
          _headerButton(
            icon: Icons.keyboard_arrow_down_rounded,
            tooltip: tr('下一个匹配'),
            disabled: _matches.isEmpty,
            onTap: () => _stepMatch(1),
          ),
          _headerButton(
            icon: Icons.close_rounded,
            tooltip: tr('关闭搜索'),
            onTap: _closeSearch,
          ),
        ],
      ),
    );
  }

  Widget _buildBody() {
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final double width = math.max(
          0,
          constraints.maxWidth - _kBodyPadding * 2,
        );
        final double height = math.max(
          0,
          constraints.maxHeight - _kBodyPadding * 2,
        );
        final int columns = (width / _cellWidth).floor().clamp(1, 500);
        final int rows = (height / _lineHeight).floor().clamp(1, 200);
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
        final Set<int> matchLines = _searching
            ? _matches.toSet()
            : const <int>{};
        return Column(
          children: <Widget>[
            if (notice.isNotEmpty) _buildNotice(notice),
            Expanded(
              child: Stack(
                children: <Widget>[
                  ListView.builder(
                    controller: _scroll,
                    padding: const EdgeInsets.symmetric(
                      horizontal: _kBodyPadding,
                      vertical: _kBodyPadding,
                    ),
                    itemExtent: _lineHeight,
                    itemCount: historyLength + screenRows,
                    itemBuilder: (BuildContext context, int index) {
                      final bool inHistory = index < historyLength;
                      final List<TerminalCell> cells = inHistory
                          ? history[index]
                          : _grid.lines[index - historyLength];
                      final int cursorColumn =
                          !inHistory &&
                              cursorOnScreen &&
                              index - historyLength == _grid.cursorRow
                          ? _grid.cursorColumn
                          : -1;
                      return _TerminalLine(
                        cells: cells,
                        cellWidth: _cellWidth,
                        cursorColumn: cursorColumn,
                        fontSize: _fontSize,
                        lineHeight: _lineHeight,
                        highlighted: matchLines.contains(index),
                      );
                    },
                  ),
                  if (!_follow)
                    Positioned(
                      right: _kBodyPadding,
                      bottom: _kBodyPadding,
                      child: _buildJumpToBottom(),
                    ),
                ],
              ),
            ),
          ],
        );
      },
    );
  }

  Widget _buildJumpToBottom() {
    return Tooltip(
      message: tr('回到底部'),
      child: Material(
        color: const Color(0xFF2A2C31),
        elevation: 4,
        shape: const StadiumBorder(),
        child: InkWell(
          key: const Key('terminal-bottom'),
          customBorder: const StadiumBorder(),
          onTap: _scrollToBottom,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                const Icon(
                  Icons.arrow_downward_rounded,
                  size: 14,
                  color: kAccentBright,
                ),
                const SizedBox(width: 4),
                Text(
                  tr('回到底部'),
                  style: const TextStyle(fontSize: 12, color: _kForeground),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  String _notice() {
    if (_openFailed) {
      return tr('无法打开终端：主机未授权、已有控制台，或上一个控制台尚未收尾');
    }
    if (_ended && _terminal.phase != ViewerTerminalPhase.failed) {
      final String reason = _terminal.reason;
      return reason.isEmpty
          ? tr('主机已结束该控制台')
          : tr('主机已结束该控制台：{reason}', <String, Object?>{'reason': reason});
    }
    switch (_terminal.phase) {
      case ViewerTerminalPhase.requesting:
        return '';
      case ViewerTerminalPhase.ended:
        final String reason = _terminal.reason;
        return reason.isEmpty
            ? tr('主机已结束该控制台')
            : tr('主机已结束该控制台：{reason}', <String, Object?>{'reason': reason});
      case ViewerTerminalPhase.failed:
        final String detail = _terminal.error.isNotEmpty
            ? _terminal.error
            : _terminal.reason;
        return detail.isEmpty ? tr('终端不可用（主机拒绝或权限已撤销）') : detail;
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
          const Icon(
            Icons.info_outline_rounded,
            size: 13,
            color: Colors.amberAccent,
          ),
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
          Expanded(
            child: Text(
              tr('点击面板后直接输入 · Ctrl+V 粘贴 · Esc 交给主机'),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(fontSize: 10, color: Colors.white38),
            ),
          ),
          Text(
            tr('{p3} 字节', <String, Object?>{'p3': _terminal.bytes}),
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
    required this.fontSize,
    required this.lineHeight,
    this.highlighted = false,
  });

  final List<TerminalCell> cells;
  final double cellWidth;
  final int cursorColumn;
  final double fontSize;
  final double lineHeight;
  final bool highlighted;

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
      spans.add(
        TextSpan(
          text: text.toString(),
          style: _styleFor(
            runFg ?? kTerminalDefaultColor,
            runBg ?? kTerminalDefaultColor,
            runAttrs,
            fontSize,
          ),
        ),
      );
      text = StringBuffer();
    }

    for (final TerminalCell cell in cells) {
      if (cell.continuation) {
        continue;
      }
      if (runFg != null &&
          (cell.fg != runFg || cell.bg != runBg || cell.attrs != runAttrs)) {
        flush();
      }
      runFg = cell.fg;
      runBg = cell.bg;
      runAttrs = cell.attrs;
      text.write(cell.text);
    }
    flush();

    return ColoredBox(
      color: highlighted
          ? kAccentBright.withValues(alpha: 0.14)
          : const Color(0x00000000),
      child: SizedBox(
        height: lineHeight,
        child: Stack(
          children: <Widget>[
            RichText(
              text: TextSpan(style: _baseStyleFor(fontSize), children: spans),
              maxLines: 1,
              softWrap: false,
              overflow: TextOverflow.clip,
            ),
            if (cursorColumn >= 0)
              Positioned(
                left: cursorColumn * cellWidth,
                top: 0,
                width: cellWidth,
                height: lineHeight,
                child: IgnorePointer(
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: kAccentBright.withValues(alpha: 0.42),
                      border: Border.all(
                        color: kAccentBright.withValues(alpha: 0.8),
                        width: 0.8,
                      ),
                    ),
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }

  static TextStyle _baseStyleFor(double fontSize) => TextStyle(
    fontFamily: 'Consolas',
    fontFamilyFallback: const <String>['Cascadia Mono', 'Courier New'],
    fontSize: fontSize,
    height: 1.0,
    color: _kForeground,
  );

  static TextStyle _styleFor(int fg, int bg, int attrs, double fontSize) {
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
      fontSize: fontSize,
      height: 1.0,
      color: foreground,
      backgroundColor: background == _kBackground ? null : background,
      fontWeight: (attrs & TerminalAttr.bold) != 0
          ? FontWeight.w700
          : FontWeight.w400,
      fontStyle: (attrs & TerminalAttr.italic) != 0
          ? FontStyle.italic
          : FontStyle.normal,
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
