import 'package:chast_viewer/remote_shortcuts.dart';
import 'package:chast_viewer/widgets/remote_shortcut_dialog.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('physical shortcut parsing excludes system-reserved chords', () {
    expect(RemoteShortcutBinding.isAllowedRemoteShortcutKey(0x04), isTrue);
    expect(RemoteShortcutBinding.isAllowedRemoteShortcutKey(0xe3), isFalse);
    expect(RemoteShortcutBinding.isAllowedRemoteShortcutKey(0x29), isFalse);
    expect(RemoteShortcutBinding.isReservedRemoteShortcut(0x2b, 2), isTrue);
    expect(RemoteShortcutBinding.isReservedRemoteShortcut(0x3d, 2), isTrue);
    expect(RemoteShortcutBinding.isReservedRemoteShortcut(0x29, 5), isTrue);
    expect(RemoteShortcutBinding.isReservedRemoteShortcut(0x3a, 1), isFalse);
  });

  testWidgets('records, applies and clears a physical key chord', (
    tester,
  ) async {
    List<RemoteShortcutBinding>? result;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              onPressed: () async {
                result = await showDialog<List<RemoteShortcutBinding>>(
                  context: context,
                  builder: (_) => const RemoteShortcutDialog(
                    initialBindings: <RemoteShortcutBinding>[],
                  ),
                );
              },
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    await tester.tap(find.byTooltip('录制快捷键').first);
    await tester.pumpAndSettle();
    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.f1);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.f1);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    await tester.pumpAndSettle();
    expect(find.text('Ctrl+F1'), findsOneWidget);

    await tester.tap(find.byTooltip('清除快捷键').first);
    await tester.pumpAndSettle();
    expect(find.text('Ctrl+F1'), findsNothing);
    await tester.tap(find.text('应用'));
    await tester.pumpAndSettle();
    expect(result, isEmpty);
  });

  testWidgets('rejects duplicate chords and Escape cancels recording', (
    tester,
  ) async {
    const RemoteShortcutBinding initial = RemoteShortcutBinding(
      action: RemoteShortcutAction.toggleControl,
      keyUsage: 0x3a,
      modifiers: 1,
      keyLabel: 'F1',
    );
    await tester.pumpWidget(
      const MaterialApp(
        home: Scaffold(
          body: RemoteShortcutDialog(
            initialBindings: <RemoteShortcutBinding>[initial],
          ),
        ),
      ),
    );
    await tester.tap(find.byTooltip('录制快捷键').at(1));
    await tester.pumpAndSettle();
    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.f1);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.f1);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    await tester.pumpAndSettle();
    expect(find.text('此快捷键已分配给其他操作'), findsOneWidget);

    await tester.sendKeyDownEvent(LogicalKeyboardKey.escape);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    expect(find.text('正在录制…'), findsNothing);
  });

  testWidgets('AltGr does not get captured as a shortcut modifier', (
    tester,
  ) async {
    await tester.pumpWidget(
      const MaterialApp(
        home: Scaffold(
          body: RemoteShortcutDialog(
            initialBindings: <RemoteShortcutBinding>[],
          ),
        ),
      ),
    );
    await tester.tap(find.byTooltip('录制快捷键').first);
    await tester.pumpAndSettle();
    await tester.sendKeyDownEvent(LogicalKeyboardKey.altRight);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyQ);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.keyQ);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.altRight);
    await tester.pumpAndSettle();

    expect(find.text('快捷键至少需要 Ctrl、Alt 或 Shift'), findsOneWidget);
    expect(find.text('Alt+Q'), findsNothing);
  });
}
