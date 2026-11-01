import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/widgets/chat_panel.dart';

Widget _host({
  required FocusNode input,
  List<ChatEntry> entries = const <ChatEntry>[],
  int sent = 0,
  int received = 0,
  Future<bool> Function(String)? onSend,
  VoidCallback? onClose,
}) {
  return MaterialApp(
    home: Scaffold(
      backgroundColor: Colors.black,
      body: Center(
        child: ChatPanel(
          entries: entries,
          sent: sent,
          received: received,
          onSend: onSend ?? (_) async => true,
          onClose: onClose ?? () {},
          inputFocusNode: input,
        ),
      ),
    ),
  );
}

void main() {
  test('accepts text up to the byte limit and rejects the rest', () {
    expect(ChatPanel.validate('hello'), isNull);
    expect(ChatPanel.validate('   '), isNotNull);
    expect(ChatPanel.validate('a\u0000b'), isNotNull);
    expect(ChatPanel.validate('a' * kMaxChatBytes), isNull);
    expect(ChatPanel.validate('a' * (kMaxChatBytes + 1)), isNotNull);
    // The limit counts UTF-8 bytes, not characters.
    expect(ChatPanel.validate('\u4e2d' * 1365), isNull);
    expect(ChatPanel.validate('\u4e2d' * 1366), isNotNull);
  });

  testWidgets('Enter trims and sends, then clears the input', (
    WidgetTester tester,
  ) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);
    final List<String> sent = <String>[];

    await tester.pumpWidget(
      _host(
        input: input,
        onSend: (String text) async {
          sent.add(text);
          return true;
        },
      ),
    );
    await tester.enterText(find.byType(TextField), '  hello host  ');
    await tester.testTextInput.receiveAction(TextInputAction.send);
    await tester.pumpAndSettle();

    expect(sent, <String>['hello host']);
    final TextField field = tester.widget<TextField>(find.byType(TextField));
    expect(field.controller!.text, isEmpty);
  });

  testWidgets('a refused send keeps the text and explains itself', (
    WidgetTester tester,
  ) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);

    await tester.pumpWidget(_host(input: input, onSend: (_) async => false));
    await tester.enterText(find.byType(TextField), 'still here');
    await tester.testTextInput.receiveAction(TextInputAction.send);
    await tester.pumpAndSettle();

    expect(find.textContaining('发送失败'), findsOneWidget);
    final TextField field = tester.widget<TextField>(find.byType(TextField));
    expect(field.controller!.text, 'still here');
  });

  testWidgets('an empty message is refused without calling the transport', (
    WidgetTester tester,
  ) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);
    int calls = 0;

    await tester.pumpWidget(
      _host(
        input: input,
        onSend: (_) async {
          ++calls;
          return true;
        },
      ),
    );
    await tester.enterText(find.byType(TextField), '   ');
    await tester.testTextInput.receiveAction(TextInputAction.send);
    await tester.pumpAndSettle();

    expect(calls, 0);
    expect(find.text('消息不能为空'), findsOneWidget);
  });

  testWidgets('renders both directions, timestamps and the counters', (
    WidgetTester tester,
  ) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);

    await tester.pumpWidget(
      _host(
        input: input,
        sent: 2,
        received: 1,
        entries: <ChatEntry>[
          ChatEntry(
            text: 'from host',
            mine: false,
            at: DateTime(2026, 1, 2, 3, 4),
          ),
          ChatEntry(
            text: 'from me',
            mine: true,
            at: DateTime(2026, 1, 2, 5, 6),
          ),
        ],
      ),
    );

    expect(find.text('收 1 · 发 2'), findsOneWidget);
    expect(find.text('from host'), findsOneWidget);
    expect(find.text('from me'), findsOneWidget);
    expect(find.text('主机 · 03:04'), findsOneWidget);
    expect(find.text('我 · 05:06'), findsOneWidget);
  });

  testWidgets('the close button reports back', (WidgetTester tester) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);
    int closes = 0;

    await tester.pumpWidget(_host(input: input, onClose: () => ++closes));
    await tester.tap(find.byTooltip('关闭聊天'));
    expect(closes, 1);
  });

  testWidgets('copies the whole conversation', (WidgetTester tester) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);
    final List<String> copies = <String>[];
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

    await tester.pumpWidget(
      _host(
        input: input,
        entries: <ChatEntry>[
          ChatEntry(
            text: 'from host',
            mine: false,
            at: DateTime(2026, 1, 2, 3, 4),
          ),
          ChatEntry(
            text: 'from me',
            mine: true,
            at: DateTime(2026, 1, 2, 5, 6),
          ),
        ],
      ),
    );
    await tester.tap(find.byTooltip('复制聊天记录（含双方消息）'));
    await tester.pump();

    expect(copies, hasLength(1));
    expect(copies.single, contains('主机 · 03:04: from host'));
    expect(copies.single, contains('我 · 05:06: from me'));
  });

  testWidgets('reading history keeps the position and offers jump-to-bottom', (
    WidgetTester tester,
  ) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);
    List<ChatEntry> entries = List<ChatEntry>.generate(
      40,
      (int i) => ChatEntry(
        text: 'message $i',
        mine: i.isEven,
        at: DateTime(2026, 11, 1, 9, i),
      ),
    );

    await tester.pumpWidget(_host(input: input, entries: entries));
    await tester.pump();

    expect(find.byTooltip('回到底部'), findsNothing);

    final ScrollController controller = tester
        .widget<ListView>(find.byType(ListView))
        .controller!;
    controller.position.jumpTo(controller.position.maxScrollExtent - 120);
    await tester.pump();
    expect(find.byTooltip('回到底部'), findsOneWidget);
    final double offset = controller.position.pixels;

    entries = <ChatEntry>[
      ...entries,
      ChatEntry(
        text: 'late message',
        mine: false,
        at: DateTime(2026, 11, 1, 10),
      ),
    ];
    await tester.pumpWidget(_host(input: input, entries: entries, received: 1));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));

    expect(controller.position.pixels, offset);
    expect(find.byTooltip('回到底部'), findsOneWidget);

    await tester.tap(find.byKey(const Key('chat-bottom')));
    await tester.pumpAndSettle();
    expect(find.byTooltip('回到底部'), findsNothing);
    expect(controller.position.pixels, controller.position.maxScrollExtent);
  });
}
