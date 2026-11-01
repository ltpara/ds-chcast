import 'dart:convert';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/terminal_panel.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('opens at the layout size and renders console output', (
    WidgetTester tester,
  ) async {
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

    await tester.pumpWidget(
      _host(
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
      ),
    );
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

  testWidgets('sends console bytes for keys while focused', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<String> sent = <String>[];

    await tester.pumpWidget(
      _host(
        focus: focus,
        status: const ViewerStatus(
          terminalSupported: true,
          terminalPhase: ViewerTerminalPhase.open,
        ),
        onSendInput: (String input) async {
          sent.add(input);
          return true;
        },
      ),
    );
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

  testWidgets('shows the host ending and stops typing', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode('bye')),
        ended: true,
      ),
    ];
    final List<String> sent = <String>[];

    await tester.pumpWidget(
      _host(
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
      ),
    );
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

  testWidgets('font buttons change the requested console size', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<int> columns = <int>[];

    await tester.pumpWidget(
      _host(
        focus: focus,
        status: const ViewerStatus(
          terminalSupported: true,
          terminalPhase: ViewerTerminalPhase.open,
        ),
        onOpen: (int width, int rows) async {
          columns.add(width);
          return true;
        },
        onResize: (int width, int rows) async {
          columns.add(width);
          return true;
        },
      ),
    );
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();
    expect(columns, isNotEmpty);
    final int before = columns.last;

    await tester.tap(find.byTooltip('放大字号'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 150));
    await tester.pump();
    expect(columns.length, greaterThan(1));
    expect(columns.last, lessThan(before));
    final int enlarged = columns.last;

    await tester.tap(find.byTooltip('缩小字号'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 150));
    await tester.pump();
    expect(columns.last, greaterThan(enlarged));
    expect(columns.last, before);
  });

  testWidgets('starts at the remembered font size and reports changes', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<int> sizes = <int>[];

    await tester.pumpWidget(
      _host(
        focus: focus,
        status: const ViewerStatus(
          terminalSupported: true,
          terminalPhase: ViewerTerminalPhase.open,
        ),
        initialFontSize: 18,
        onFontSizeChanged: sizes.add,
      ),
    );
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();

    await tester.tap(find.byTooltip('缩小字号'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 150));
    await tester.pump();
    expect(sizes, <int>[17]);

    await tester.tap(find.byTooltip('放大字号'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 150));
    await tester.pump();
    expect(sizes, <int>[17, 18]);
  });

  testWidgets('shows a jump-to-bottom button after scrolling up', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final StringBuffer output = StringBuffer();
    for (int i = 1; i <= 60; i++) {
      output.write('row $i');
      if (i != 60) {
        output.write('\r\n');
      }
    }
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode(output.toString())),
        ended: false,
      ),
    ];

    await tester.pumpWidget(
      _host(
        focus: focus,
        status: const ViewerStatus(
          terminalSupported: true,
          terminalPhase: ViewerTerminalPhase.open,
        ),
        onTakeOutput: () async => queue.isEmpty ? null : queue.removeAt(0),
      ),
    );
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();

    expect(find.byTooltip('回到底部'), findsNothing);

    await tester.drag(find.byType(ListView), const Offset(0, 200));
    await tester.pump();
    expect(find.byTooltip('回到底部'), findsOneWidget);

    await tester.tap(find.byKey(const Key('terminal-bottom')));
    await tester.pump();
    expect(find.byTooltip('回到底部'), findsNothing);

    final ScrollableState scrollable = tester.state<ScrollableState>(
      find.descendant(
        of: find.byType(ListView),
        matching: find.byType(Scrollable),
      ),
    );
    expect(scrollable.position.pixels, scrollable.position.maxScrollExtent);
  });

  testWidgets(
    'Shift+PageUp pages through history and PageDown resumes following',
    (WidgetTester tester) async {
      final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
      addTearDown(focus.dispose);
      final StringBuffer output = StringBuffer();
      for (int i = 1; i <= 60; i++) {
        output.write('row $i');
        if (i != 60) {
          output.write('\r\n');
        }
      }
      final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
        ViewerTerminalOutput(
          data: Uint8List.fromList(utf8.encode(output.toString())),
          ended: false,
        ),
      ];
      final List<String> sent = <String>[];

      await tester.pumpWidget(
        _host(
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
        ),
      );
      await tester.pump();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
      await tester.pump();
      focus.requestFocus();
      await tester.pump();

      final ScrollPosition position = tester
          .state<ScrollableState>(
            find.descendant(
              of: find.byType(ListView),
              matching: find.byType(Scrollable),
            ),
          )
          .position;
      expect(position.pixels, position.maxScrollExtent);

      Future<void> press(LogicalKeyboardKey key) async {
        await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
        await tester.sendKeyDownEvent(key);
        await tester.sendKeyUpEvent(key);
        await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
        await tester.pump();
      }

      await press(LogicalKeyboardKey.pageUp);
      expect(position.pixels, lessThan(position.maxScrollExtent));
      expect(find.byTooltip('回到底部'), findsOneWidget);

      for (int i = 0; i < 10; i++) {
        await press(LogicalKeyboardKey.pageDown);
      }
      expect(position.pixels, position.maxScrollExtent);
      expect(find.byTooltip('回到底部'), findsNothing);
      // Shift+Page keys scroll locally instead of reaching the console.
      expect(sent, isEmpty);
    },
  );

  testWidgets('Shift+Home and Shift+End jump to the history edges', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final StringBuffer output = StringBuffer();
    for (int i = 1; i <= 60; i++) {
      output.write('row $i');
      if (i != 60) {
        output.write('\r\n');
      }
    }
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode(output.toString())),
        ended: false,
      ),
    ];

    await tester.pumpWidget(
      _host(
        focus: focus,
        status: const ViewerStatus(
          terminalSupported: true,
          terminalPhase: ViewerTerminalPhase.open,
        ),
        onTakeOutput: () async => queue.isEmpty ? null : queue.removeAt(0),
      ),
    );
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();
    focus.requestFocus();
    await tester.pump();

    final ScrollPosition position = tester
        .state<ScrollableState>(
          find.descendant(
            of: find.byType(ListView),
            matching: find.byType(Scrollable),
          ),
        )
        .position;
    expect(position.pixels, position.maxScrollExtent);

    Future<void> press(LogicalKeyboardKey key) async {
      await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
      await tester.sendKeyDownEvent(key);
      await tester.sendKeyUpEvent(key);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
      await tester.pump();
    }

    await press(LogicalKeyboardKey.home);
    expect(position.pixels, 0);
    expect(find.byTooltip('回到底部'), findsOneWidget);

    await press(LogicalKeyboardKey.end);
    expect(position.pixels, position.maxScrollExtent);
    expect(find.byTooltip('回到底部'), findsNothing);
  });

  testWidgets('searches console history and steps between matches', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final StringBuffer output = StringBuffer();
    for (int i = 1; i <= 60; i++) {
      output.write('row $i');
      if (i == 10 || i == 40) {
        output.write(' needle');
      }
      if (i != 60) {
        output.write('\r\n');
      }
    }
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode(output.toString())),
        ended: false,
      ),
    ];

    await tester.pumpWidget(
      _host(
        focus: focus,
        status: const ViewerStatus(
          terminalSupported: true,
          terminalPhase: ViewerTerminalPhase.open,
        ),
        onTakeOutput: () async => queue.isEmpty ? null : queue.removeAt(0),
      ),
    );
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();

    await tester.tap(find.byTooltip('搜索终端内容（Ctrl+F）'));
    await tester.pump();
    expect(find.byKey(const Key('terminal-search-field')), findsOneWidget);

    await tester.enterText(
      find.byKey(const Key('terminal-search-field')),
      'NEEDLE',
    );
    await tester.pump();
    expect(_searchCount(tester), '2/2');

    await tester.tap(find.byTooltip('上一个匹配'));
    await tester.pump();
    expect(_searchCount(tester), '1/2');

    await tester.tap(find.byTooltip('下一个匹配'));
    await tester.pump();
    expect(_searchCount(tester), '2/2');

    await tester.tap(find.byTooltip('关闭搜索'));
    await tester.pump();
    expect(find.byKey(const Key('terminal-search-field')), findsNothing);
  });

  testWidgets('copies console contents including scrollback', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<String> copies = <String>[];
    _mockClipboard(tester, copies);
    final StringBuffer output = StringBuffer();
    for (int i = 1; i <= 40; i++) {
      output.write('row $i');
      if (i != 40) {
        output.write('\r\n');
      }
    }
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode(output.toString())),
        ended: false,
      ),
    ];

    await tester.pumpWidget(
      _host(
        focus: focus,
        status: const ViewerStatus(
          terminalSupported: true,
          terminalPhase: ViewerTerminalPhase.open,
        ),
        onTakeOutput: () async => queue.isEmpty ? null : queue.removeAt(0),
      ),
    );
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();

    await tester.tap(find.byTooltip('复制终端内容（Ctrl+Shift+C，含滚动历史）'));
    await tester.pump();
    await tester.pump();

    expect(copies, hasLength(1));
    expect(copies.single, contains('row 1\n'));
    expect(copies.single, contains('row 40'));
    expect(find.text('已复制终端内容'), findsOneWidget);
  });

  testWidgets('Ctrl+Shift+C copies instead of sending Ctrl+C', (
    WidgetTester tester,
  ) async {
    final FocusNode focus = FocusNode(debugLabel: 'terminal-test');
    addTearDown(focus.dispose);
    final List<String> copies = <String>[];
    _mockClipboard(tester, copies);
    final List<String> sent = <String>[];
    final List<ViewerTerminalOutput> queue = <ViewerTerminalOutput>[
      ViewerTerminalOutput(
        data: Uint8List.fromList(utf8.encode('prompt>')),
        ended: false,
      ),
    ];

    await tester.pumpWidget(
      _host(
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
      ),
    );
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump();

    focus.requestFocus();
    await tester.pump();
    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyC);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.keyC);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    await tester.pump();
    await tester.pump();

    expect(copies, hasLength(1));
    expect(copies.single, contains('prompt>'));
    expect(sent, isEmpty);
  });
}

void _mockClipboard(WidgetTester tester, List<String> copies) {
  tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
    SystemChannels.platform,
    (MethodCall call) async {
      if (call.method == 'Clipboard.setData') {
        copies.add(
          (call.arguments as Map<Object?, Object?>)['text']! as String,
        );
      }
      return null;
    },
  );
  addTearDown(
    () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      null,
    ),
  );
}

Widget _host({
  required FocusNode focus,
  required ViewerStatus status,
  Future<bool> Function(int columns, int rows)? onOpen,
  Future<ViewerTerminalOutput?> Function()? onTakeOutput,
  Future<bool> Function(String input)? onSendInput,
  Future<bool> Function(int columns, int rows)? onResize,
  int initialFontSize = 13,
  ValueChanged<int>? onFontSizeChanged,
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
            initialFontSize: initialFontSize,
            onFontSizeChanged: onFontSizeChanged,
          ),
        ),
      ),
    ),
  );
}

String _searchCount(WidgetTester tester) {
  return tester
          .widget<Text>(find.byKey(const Key('terminal-search-count')))
          .data ??
      '';
}

String _consoleText(WidgetTester tester) {
  final StringBuffer out = StringBuffer();
  for (final RichText rich in tester.widgetList<RichText>(
    find.byType(RichText),
  )) {
    out.write(rich.text.toPlainText());
    out.write('\n');
  }
  return out.toString();
}
