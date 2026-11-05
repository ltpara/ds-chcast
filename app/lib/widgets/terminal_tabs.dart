import 'package:flutter/material.dart';

import '../l10n.dart';
import '../viewer_bridge.dart';
import 'terminal_panel.dart';

/// Each tab keeps its screen and output poll while another tab has focus.
/// Removing the tab disposes its panel, which closes only that host console.
class TerminalTabs extends StatefulWidget {
  const TerminalTabs({
    super.key,
    required this.status,
    required this.focusNode,
    required this.onDismiss,
    this.initialFontSize = 13,
    this.onFontSizeChanged,
  });

  final ViewerStatus status;
  final FocusNode focusNode;
  final VoidCallback onDismiss;
  final int initialFontSize;
  final ValueChanged<int>? onFontSizeChanged;

  @override
  State<TerminalTabs> createState() => _TerminalTabsState();
}

class _TerminalTab {
  _TerminalTab(this.id) : focus = FocusNode(debugLabel: 'terminal-$id');

  final int id;
  final FocusNode focus;
}

class _TerminalTabsState extends State<TerminalTabs> {
  final List<_TerminalTab> _tabs = <_TerminalTab>[_TerminalTab(1)];
  int _selected = 1;

  @override
  void initState() {
    super.initState();
    widget.focusNode.addListener(_focusSelected);
    WidgetsBinding.instance.addPostFrameCallback((_) => _focusSelected());
  }

  @override
  void dispose() {
    widget.focusNode.removeListener(_focusSelected);
    for (final _TerminalTab tab in _tabs) {
      tab.focus.dispose();
    }
    super.dispose();
  }

  void _focusSelected() {
    if (!mounted || !widget.focusNode.hasPrimaryFocus) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && widget.focusNode.hasFocus) {
        _tabs.firstWhere((tab) => tab.id == _selected).focus.requestFocus();
      }
    });
  }

  void _select(_TerminalTab tab) {
    setState(() => _selected = tab.id);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _selected == tab.id && _tabs.contains(tab)) {
        tab.focus.requestFocus();
      }
    });
  }

  void _add() {
    if (_tabs.length >= kViewerMaxTerminals || !widget.status.acceptsTerminal) {
      return;
    }
    final int id = List<int>.generate(
      kViewerMaxTerminals,
      (i) => i + 1,
    ).firstWhere((id) => !_tabs.any((tab) => tab.id == id));
    final _TerminalTab tab = _TerminalTab(id);
    setState(() => _tabs.add(tab));
    _select(tab);
  }

  void _close(_TerminalTab tab) {
    if (_tabs.length == 1) {
      widget.onDismiss();
      return;
    }
    setState(() {
      _tabs.remove(tab);
      if (_selected == tab.id) _selected = _tabs.first.id;
    });
    // Wait until the panel using this focus node has been unmounted.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      tab.focus.dispose();
      if (mounted) {
        _tabs.firstWhere((tab) => tab.id == _selected).focus.requestFocus();
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    return Focus(
      focusNode: widget.focusNode,
      child: Column(
        children: <Widget>[
          Material(
            color: const Color(0xFF1B1C20),
            borderRadius: const BorderRadius.vertical(top: Radius.circular(8)),
            child: Row(
              children: <Widget>[
                Expanded(
                  child: SingleChildScrollView(
                    scrollDirection: Axis.horizontal,
                    child: Row(
                      children: <Widget>[
                        for (final _TerminalTab tab in _tabs)
                          Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 4),
                            child: ChoiceChip(
                              label: Text(
                                tr('终端 {id}', <String, Object?>{'id': tab.id}),
                              ),
                              selected: _selected == tab.id,
                              onSelected: (_) => _select(tab),
                            ),
                          ),
                      ],
                    ),
                  ),
                ),
                IconButton(
                  tooltip: tr('新建终端'),
                  icon: const Icon(Icons.add_rounded),
                  onPressed:
                      _tabs.length < kViewerMaxTerminals &&
                          widget.status.acceptsTerminal
                      ? _add
                      : null,
                ),
                IconButton(
                  tooltip: tr('关闭所有终端'),
                  icon: const Icon(Icons.close_rounded),
                  onPressed: widget.onDismiss,
                ),
              ],
            ),
          ),
          Expanded(
            child: Stack(
              fit: StackFit.expand,
              children: <Widget>[
                for (final _TerminalTab tab in _tabs)
                  Offstage(
                    key: ObjectKey(tab),
                    offstage: tab.id != _selected,
                    child: ExcludeFocus(
                      excluding: tab.id != _selected,
                      child: TerminalPanel(
                        status: widget.status,
                        terminalId: tab.id,
                        focusNode: tab.focus,
                        onOpen: (columns, rows) => ViewerBridge.openTerminal(
                          columns: columns,
                          rows: rows,
                          terminalId: tab.id,
                        ),
                        onTakeOutput: () =>
                            ViewerBridge.takeTerminalOutput(terminalId: tab.id),
                        onSendInput: (input) => ViewerBridge.sendTerminalInput(
                          input,
                          terminalId: tab.id,
                        ),
                        onResize: (columns, rows) =>
                            ViewerBridge.resizeTerminal(
                              columns: columns,
                              rows: rows,
                              terminalId: tab.id,
                            ),
                        onClose: () =>
                            ViewerBridge.closeTerminal(terminalId: tab.id),
                        onDismiss: () => _close(tab),
                        initialFontSize: widget.initialFontSize,
                        onFontSizeChanged: widget.onFontSizeChanged,
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
