import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/widgets/send_text_dialog.dart';

Future<List<String>> _open(
  WidgetTester tester, {
  required Future<bool> Function(String text) onSend,
}) async {
  final List<String> sent = <String>[];
  await tester.pumpWidget(
    MaterialApp(
      home: Scaffold(
        body: Builder(
          builder: (BuildContext context) {
            return TextButton(
              onPressed: () {
                showDialog<bool>(
                  context: context,
                  builder: (BuildContext dialogContext) => SendTextDialog(
                    onSend: (String text) {
                      sent.add(text);
                      return onSend(text);
                    },
                  ),
                );
              },
              child: const Text('open'),
            );
          },
        ),
      ),
    ),
  );
  await tester.tap(find.text('open'));
  await tester.pumpAndSettle();
  return sent;
}

void main() {
  test('accepts text up to the byte limit and keeps whitespace', () {
    expect(SendTextDialog.validate('hello'), isNull);
    // A newline is a useful block: it presses Return on the host.
    expect(SendTextDialog.validate('\n'), isNull);
    expect(SendTextDialog.validate('  '), isNull);
    expect(SendTextDialog.validate(''), isNotNull);
    expect(SendTextDialog.validate('a\u0000b'), isNotNull);
    expect(SendTextDialog.validate('a' * kMaxTextInputBytes), isNull);
    expect(SendTextDialog.validate('a' * (kMaxTextInputBytes + 1)), isNotNull);
    // The limit counts UTF-8 bytes, not characters.
    expect(SendTextDialog.validate('\u4e2d' * 1365), isNull);
    expect(SendTextDialog.validate('\u4e2d' * 1366), isNotNull);
  });

  testWidgets('sends the composed text and closes', (
    WidgetTester tester,
  ) async {
    const String text = '订单 42\n第二行 😀';
    final List<String> sent = await _open(tester, onSend: (_) async => true);

    await tester.enterText(find.byType(TextField), text);
    await tester.tap(find.text('发送'));
    await tester.pumpAndSettle();

    expect(sent, <String>[text]);
    expect(find.byType(SendTextDialog), findsNothing);
  });

  testWidgets('a refused send keeps the dialog and the text', (
    WidgetTester tester,
  ) async {
    final List<String> sent = await _open(tester, onSend: (_) async => false);

    await tester.enterText(find.byType(TextField), 'hello host');
    await tester.tap(find.text('发送'));
    await tester.pumpAndSettle();

    expect(sent, <String>['hello host']);
    expect(find.byType(SendTextDialog), findsOneWidget);
    expect(find.textContaining('发送失败'), findsOneWidget);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller?.text,
      'hello host',
    );
  });

  testWidgets('Cancel closes without sending', (WidgetTester tester) async {
    final List<String> sent = await _open(tester, onSend: (_) async => true);

    await tester.enterText(find.byType(TextField), 'never sent');
    await tester.tap(find.text('取消'));
    await tester.pumpAndSettle();

    expect(sent, isEmpty);
    expect(find.byType(SendTextDialog), findsNothing);
  });
}
