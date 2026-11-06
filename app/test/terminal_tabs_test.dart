import 'dart:convert';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/terminal_tabs.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');

  test('native status retains separate terminal states per slot', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'terminalPhase': 2,
      'terminalColumns': 80,
      'terminals': <Object?>[
        <Object?, Object?>{'phase': 2, 'columns': 80, 'bytes': 42, 'serviceId': 7},
        <Object?, Object?>{'phase': 4, 'reason': 'revoked'},
      ],
    });
    expect(status.terminal(1).phase, ViewerTerminalPhase.open);
    expect(status.terminal(1).columns, 80);
    expect(status.terminal(1).bytes, 42);
    expect(status.terminal(1).serviceId, 7);
    expect(status.terminal(2).phase, ViewerTerminalPhase.failed);
    expect(status.terminal(2).reason, 'revoked');
    expect(status.terminal(3).phase, ViewerTerminalPhase.closed);
  });

  testWidgets(
    'tabs retain output, route keys and close only their own console',
    (WidgetTester tester) async {
      tester.view.physicalSize = const Size(1200, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final FocusNode focus = FocusNode();
      addTearDown(focus.dispose);
      final List<int> opens = <int>[];
      final List<int> closes = <int>[];
      final List<int> inputs = <int>[];
      final List<List<int>> tabChanges = <List<int>>[];
      final Map<int, List<Uint8List>> output = <int, List<Uint8List>>{};
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (MethodCall call) async {
            final Map<Object?, Object?> args =
                call.arguments as Map<Object?, Object?>;
            final int id = args['terminalId']! as int;
            switch (call.method) {
              case 'openTerminal':
                opens.add(id);
                output[id] = <Uint8List>[
                  Uint8List.fromList(utf8.encode('output-from-tab-$id')),
                ];
                return true;
              case 'sendTerminalInput':
                inputs.add(id);
                return true;
              case 'resizeTerminal':
                return true;
              case 'closeTerminal':
                closes.add(id);
                return null;
              case 'takeTerminalOutput':
                final List<Uint8List>? queue = output[id];
                return <String, Object>{
                  'data': queue == null || queue.isEmpty
                      ? Uint8List(0)
                      : queue.removeAt(0),
                  'ended': false,
                };
            }
            return null;
          });
      addTearDown(() {
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null);
      });
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: TerminalTabs(
              status: const ViewerStatus(
                state: ViewerState.streaming,
                terminalSupported: true,
                terminals: <ViewerTerminalStatus>[
                  ViewerTerminalStatus(phase: ViewerTerminalPhase.open),
                  ViewerTerminalStatus(phase: ViewerTerminalPhase.open),
                ],
              ),
              focusNode: focus,
              onDismiss: () {},
              onTabsChanged: tabChanges.add,
            ),
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 60));
      expect(opens, <int>[1]);
      await tester.tap(find.byTooltip('新建终端'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 60));
      expect(opens, <int>[1, 2]);
      expect(tabChanges, <List<int>>[
        <int>[1, 2],
      ]);
      expect(closes, isEmpty);
      expect(
        tester
            .widget<IconButton>(
              find.widgetWithIcon(IconButton, Icons.add_rounded),
            )
            .onPressed,
        isNull,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.pump();
      expect(inputs, <int>[2]);

      await tester.tap(find.text('终端 1'));
      await tester.pump();
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pump();
      expect(inputs, <int>[2, 1]);
      final String visibleText = tester
          .widgetList<RichText>(find.byType(RichText))
          .map((RichText widget) => widget.text.toPlainText())
          .join('\n');
      expect(visibleText, contains('output-from-tab-1'));
      expect(visibleText, isNot(contains('output-from-tab-2')));
      expect(opens, <int>[1, 2]);

      await tester.tap(find.byTooltip('关闭终端（同时结束主机上的控制台）'));
      await tester.pump();
      expect(closes, <int>[1]);
      expect(tabChanges.last, <int>[2]);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.pump();
      expect(inputs.last, 2);
      // Disposal is not a close: a persistent console must survive the
      // widgets a reconnect tears down, so only the tab's own stop was sent.
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
      expect(closes, <int>[1]);
    },
  );
}
