import 'package:flutter/material.dart';
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

  testWidgets('Enter trims and sends, then clears the input', (WidgetTester tester) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);
    final List<String> sent = <String>[];

    await tester.pumpWidget(_host(
      input: input,
      onSend: (String text) async {
        sent.add(text);
        return true;
      },
    ));
    await tester.enterText(find.byType(TextField), '  hello host  ');
    await tester.testTextInput.receiveAction(TextInputAction.send);
    await tester.pumpAndSettle();

    expect(sent, <String>['hello host']);
    final TextField field = tester.widget<TextField>(find.byType(TextField));
    expect(field.controller!.text, isEmpty);
  });

  testWidgets('a refused send keeps the text and explains itself',
      (WidgetTester tester) async {
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

  testWidgets('an empty message is refused without calling the transport',
      (WidgetTester tester) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);
    int calls = 0;

    await tester.pumpWidget(_host(
      input: input,
      onSend: (_) async {
        ++calls;
        return true;
      },
    ));
    await tester.enterText(find.byType(TextField), '   ');
    await tester.testTextInput.receiveAction(TextInputAction.send);
    await tester.pumpAndSettle();

    expect(calls, 0);
    expect(find.text('消息不能为空'), findsOneWidget);
  });

  testWidgets('renders both directions, timestamps and the counters',
      (WidgetTester tester) async {
    final FocusNode input = FocusNode();
    addTearDown(input.dispose);

    await tester.pumpWidget(_host(
      input: input,
      sent: 2,
      received: 1,
      entries: <ChatEntry>[
        ChatEntry(text: 'from host', mine: false, at: DateTime(2026, 1, 2, 3, 4)),
        ChatEntry(text: 'from me', mine: true, at: DateTime(2026, 1, 2, 5, 6)),
      ],
    ));

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
}
