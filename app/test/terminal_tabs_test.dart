import 'dart:convert';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/terminal_tabs.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');

  test('native status retains separate terminal states and input-block result', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'blockInputSupported': true,
      'blockInputEnabled': true,
      'blockInputCode': 3,
      'blockInputError': 'release failed',
      'lockAfterSessionSupported': true,
      'lockAfterSessionEnabled': false,
      'lockAfterSessionCode': 1,
      'lockAfterSessionError': 'permission revoked',
      'lockAfterSessionUpdates': 3,
      'terminals': <Object?>[
        <Object?, Object?>{'phase': 2, 'columns': 80, 'bytes': 42},
        <Object?, Object?>{'phase': 4, 'reason': 'revoked'},
      ],
    });
    expect(status.terminal(1).phase, ViewerTerminalPhase.open);
    expect(status.terminal(1).columns, 80);
    expect(status.terminal(1).bytes, 42);
    expect(status.terminal(2).phase, ViewerTerminalPhase.failed);
    expect(status.terminal(2).reason, 'revoked');
    expect(status.blockInputEnabled, isTrue);
    expect(status.blockInputCode, 3);
    expect(status.lockAfterSessionSupported, isTrue);
    expect(status.lockAfterSessionEnabled, isFalse);
    expect(status.lockAfterSessionCode, 1);
    expect(status.lockAfterSessionError, 'permission revoked');
    expect(status.lockAfterSessionUpdates, 3);
  });

  test('display bridge preserves command and authoritative failure state', () async {
    final status = ViewerStatus.fromMap(<Object?, Object?>{
      'displayControlSupported': true, 'displayControlManaged': true,
      'displayControlCount': 2, 'displayControlPrivacy': 1,
      'displayControlCode': 4, 'displayControlError': 'recovery pending',
      'displayControlUpdates': 7, 'displayControlBusy': false,
      'displayControlOutputs': <Object>[<String, Object>{'id': 31, 'active': true}],
    });
    expect(status.displayControlSupported, isTrue);
    expect(status.displayControlManaged, isTrue);
    expect(status.displayControlCount, 2);
    expect(status.displayControlPrivacy, 1);
    expect(status.displayControlCode, 4);
    expect(status.displayControlError, 'recovery pending');
    expect(status.displayControlUpdates, 7);
    expect(status.displayControlOutputs.single.id, 31);
    expect(status.displayControlOutputs.single.active, isTrue);
    final List<Object?> requests = <Object?>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          expect(call.method, 'controlDisplays'); requests.add(call.arguments); return true;
        });
    addTearDown(() => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, null));
    expect(await ViewerBridge.controlDisplays(1, 4), isTrue);
    expect(await ViewerBridge.controlDisplays(2, 0), isTrue);
    expect(await ViewerBridge.controlDisplays(4, 0, target: 31), isTrue);
    expect(requests, <Object?>[<String, int>{'action': 1, 'value': 4}, <String, int>{'action': 2, 'value': 0}, <String, int>{'action': 4, 'value': 0, 'target': 31}]);
  });

  test('lock after session bridge sends enable and cancel unchanged', () async {
    final List<bool> requests = <bool>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall call) async {
          expect(call.method, 'setLockAfterSession');
          requests.add(call.arguments as bool);
          return true;
        });
    addTearDown(() => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null));
    expect(await ViewerBridge.setLockAfterSession(true), isTrue);
    expect(await ViewerBridge.setLockAfterSession(false), isTrue);
    expect(requests, <bool>[true, false]);
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
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.pump();
      expect(inputs.last, 2);
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
      expect(closes, <int>[1, 2]);
    },
  );
}
