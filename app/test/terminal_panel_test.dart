import 'dart:convert';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/terminal_panel.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('opens at the layout size and renders console output',
      (WidgetTester tester) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode('hello\r\nworld')),
        ended: false,
      ),
    ];
    int? openedColumns;
    int? openedRows;

    await tester.pumpWidget(_host(
      focus: focus,
      status: const ViewerStatus(
        terminalSupported: true,
        terminalPhase: ViewerTerminalPhase.requesting,
      ),
      onOpen: (int columns, int rows) async {
        openedColumns = columns;
        openedRows = rows;
        return true;
      },
      onTakeOutput: () async => queue.isEmpty ? null : queue.removeAt(0),
    ));
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();

    expect(openedColumns, isNotNull);
    expect(openedColumns! > 0, isTrue);
    expect(openedRows! > 0, isTrue);
    expect(_consoleText(tester), contains('hello'));
    expect(_consoleText(tester), contains('world'));
  });

  testWidgets('sends console bytes for keys while focused',
      (WidgetTester tester) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<String> sent = <String>[];

    await tester.pumpWidget(_host(
      focus: focus,
      status: const ViewerStatus(
        terminalSupported: true,
        terminalPhase: ViewerTerminalPhase.open,
      ),
      onSendInput: (String input) async {
        sent.add(input);
        return true;
      },
    ));
    await tester.pump();
    await tester.pump();
    focus.requestFocus();
    await tester.pump();

    await tester.sendKeyDownEvent(LogicalKeyboardKey.arrowUp);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.arrowUp);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyC);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.keyC);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    await tester.pump();

    expect(sent, contains('\x1b[A'));
    expect(sent, contains('\x03'));
  });

  testWidgets('shows the host ending and stops typing',
      (WidgetTester tester) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode('bye')),
        ended: true,
      ),
    ];
    final List<String> sent = <String>[];

    await tester.pumpWidget(_host(
      focus: focus,
      status: const ViewerStatus(
        terminalSupported: true,
        terminalPhase: ViewerTerminalPhase.open,
      ),
      onTakeOutput: () async => queue.isEmpty ? null : queue.removeAt(0),
      onSendInput: (String input) async {
        sent.add(input);
        return true;
      },
    ));
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();

    expect(find.text('主机已结束该控制台'), findsOneWidget);
    expect(_consoleText(tester), contains('bye'));

    focus.requestFocus();
    await tester.pump();
    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyA);
    await tester.pump();
    expect(sent, isEmpty);
  });
}

Widget _host({
  required FocusNode focus,
  required ViewerStatus status,
  Future<bool> Function(int columns, int rows)? onOpen,
  Future<ViewerTerminalOutput?> Function()? onTakeOutput,
  Future<bool> Function(String input)? onSendInput,
  Future<bool> Function(int columns, int rows)? onResize,
}) {
  return MaterialApp(
    home: Scaffold(
      body: Center(
        child: SizedBox(
          width: 800,
          height: 500,
          child: TerminalPanel(
            status: status,
            focusNode: focus,
            onOpen: onOpen ?? (int columns, int rows) async => true,
            onTakeOutput: onTakeOutput ?? () async => null,
            onSendInput: onSendInput ?? (String input) async => true,
            onResize: onResize ?? (int columns, int rows) async => true,
            onClose: () async {},
            onDismiss: () {},
          ),
        ),
      ),
    ),
  );
}

String _consoleText(WidgetTester tester) {
  final StringBuffer out = StringBuffer();
  for (final RichText rich
      in tester.widgetList<RichText>(find.byType(RichText))) {
    out.write(rich.text.toPlainText());
    out.write('\n');
  }
  return out.toString();
}
