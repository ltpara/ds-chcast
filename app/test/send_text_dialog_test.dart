import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/widgets/send_text_dialog.dart';

final Finder _loadClipboard = find.byTooltip('从本地剪贴板取文本');

void _mockClipboard(Future<Object?> Function() read) {
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(SystemChannels.platform, (call) {
    if (call.method == 'Clipboard.getData') {
      expect(call.arguments, Clipboard.kTextPlain);
      return read();
    }
    return Future<Object?>.value();
  });
  addTearDown(() {
    messenger.setMockMethodCallHandler(SystemChannels.platform, null);
  });
}

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

  testWidgets('previews Unicode clipboard text before sending it', (
    WidgetTester tester,
  ) async {
    const String text = '  剪贴板 😀\n下一行  ';
    var reads = 0;
    _mockClipboard(() async {
      ++reads;
      return <String, String>{'text': text};
    });
    final List<String> sent = await _open(tester, onSend: (_) async => true);
    expect(reads, 0);

    await tester.tap(_loadClipboard);
    await tester.pumpAndSettle();

    expect(reads, 1);
    expect(sent, isEmpty);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller?.text,
      text,
    );

    await tester.tap(find.text('发送'));
    await tester.pumpAndSettle();

    expect(sent, <String>[text]);
    expect(find.byType(SendTextDialog), findsNothing);
  });

  testWidgets('cancel never sends a loaded clipboard', (
    WidgetTester tester,
  ) async {
    _mockClipboard(() async => <String, String>{'text': 'local only'});
    final List<String> sent = await _open(tester, onSend: (_) async => true);

    await tester.tap(_loadClipboard);
    await tester.pumpAndSettle();
    await tester.tap(find.text('取消'));
    await tester.pumpAndSettle();

    expect(sent, isEmpty);
    expect(find.byType(SendTextDialog), findsNothing);
  });

  testWidgets('invalid clipboard entries keep the current draft', (
    WidgetTester tester,
  ) async {
    final responses = <Object?>[
      <String, String>{'text': '中' * 1366},
      <String, String>{'text': 'a\u0000b'},
      null,
      PlatformException(code: 'clipboard_unavailable'),
    ].iterator;
    _mockClipboard(() async {
      expect(responses.moveNext(), isTrue);
      final Object? response = responses.current;
      if (response is PlatformException) {
        throw response;
      }
      return response;
    });
    final List<String> sent = await _open(tester, onSend: (_) async => true);
    final Finder field = find.byType(TextField);
    await tester.enterText(field, 'current draft');

    for (final String message in <String>[
      '文本过长',
      '文本不能包含空字符',
      '剪贴板没有纯文本',
      '无法读取本地剪贴板',
    ]) {
      await tester.tap(_loadClipboard);
      await tester.pumpAndSettle();

      expect(find.textContaining(message), findsOneWidget);
      expect(tester.widget<TextField>(field).controller?.text, 'current draft');
      expect(sent, isEmpty);
    }
  });

  testWidgets('a pending clipboard read preserves newer edits', (
    WidgetTester tester,
  ) async {
    final clipboard = Completer<Object?>();
    _mockClipboard(() => clipboard.future);
    final List<String> sent = await _open(tester, onSend: (_) async => true);
    final Finder field = find.byType(TextField);

    await tester.tap(_loadClipboard);
    await tester.pump();
    expect(
      tester.widget<FilledButton>(find.byType(FilledButton)).onPressed,
      isNull,
    );
    await tester.enterText(field, 'newer draft');
    clipboard.complete(<String, String>{'text': 'late clipboard'});
    await tester.pumpAndSettle();

    expect(tester.widget<TextField>(field).controller?.text, 'newer draft');
    expect(sent, isEmpty);
  });
}
