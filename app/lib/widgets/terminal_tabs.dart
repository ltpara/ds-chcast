import 'dart:async';

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
    this.initialTabs = const <int>[1],
    this.resumeServiceIds = const <int>[],
    this.onTabsChanged,
    this.initialFontSize = 13,
    this.onFontSizeChanged,
  });

  final ViewerStatus status;
  final FocusNode focusNode;
  final VoidCallback onDismiss;
  /// Console slots the widget starts with, in display order. The page uses
  /// this to restore exactly the tabs whose host console outlived a reconnect.
  final List<int> initialTabs;
  /// Slot 1..[kViewerMaxTerminals] → the host service id to attach to, 0 for
  /// a fresh console. Only a tab's first open uses it.
  final List<int> resumeServiceIds;
  /// Reports the open tab ids after a tab is added or removed so the page can
  /// keep its per-slot resume ids and close-all in sync.
  final ValueChanged<List<int>>? onTabsChanged;
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
  final List<_TerminalTab> _tabs = <_TerminalTab>[];
  int _selected = 1;

  @override
  void initState() {
    super.initState();
    final Set<int> seen = <int>{};
    for (final int id in widget.initialTabs) {
      if (id >= 1 && id <= kViewerMaxTerminals && seen.add(id)) {
        _tabs.add(_TerminalTab(id));
      }
    }
    if (_tabs.isEmpty) {
      _tabs.add(_TerminalTab(1));
    }
    _selected = _tabs.first.id;
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

  void _notifyTabs() {
    widget.onTabsChanged?.call(<int>[for (final _TerminalTab tab in _tabs) tab.id]);
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
    _notifyTabs();
    _select(tab);
  }

  void _close(_TerminalTab tab) {
    if (_tabs.length == 1) {
      // The page owns the last tab: dismissing closes every console it knows
      // about, not just this slot.
      widget.onDismiss();
      return;
    }
    // The panel no longer closes its console on dispose (a reconnect disposes
    // the widgets but keeps the service), so a user-initiated tab close sends
    // the stop here. Otherwise a persistent service would outlive its tab.
    unawaited(ViewerBridge.closeTerminal(terminalId: tab.id));
    setState(() {
      _tabs.remove(tab);
      if (_selected == tab.id) _selected = _tabs.first.id;
    });
    _notifyTabs();
    // Wait until the panel using this focus node has been unmounted.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      tab.focus.dispose();
      if (mounted) {
        _tabs.firstWhere((tab) => tab.id == _selected).focus.requestFocus();
      }
    });
  }

  int _resumeServiceId(int id) {
    final List<int> ids = widget.resumeServiceIds;
    return id >= 1 && id <= ids.length ? ids[id - 1] : 0;
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
                        resumeServiceId: _resumeServiceId(tab.id),
                        onOpen:
                            (int columns, int rows, int serviceId) =>
                                ViewerBridge.openTerminal(
                                  columns: columns,
                                  rows: rows,
                                  terminalId: tab.id,
                                  serviceId: serviceId,
                                ),
                        onTakeOutput: () =>
                            ViewerBridge.takeTerminalOutput(terminalId: tab.id),
                        onSendInput: (String input) =>
                            ViewerBridge.sendTerminalInput(
                              input,
                              terminalId: tab.id,
                            ),
                        onResize: (int columns, int rows) =>
                            ViewerBridge.resizeTerminal(
                              columns: columns,
                              rows: rows,
                              terminalId: tab.id,
                            ),
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
