import 'dart:async';
import 'package:chast_viewer/remote_shortcuts.dart';
import 'package:chast_viewer/viewer_bridge.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');
  final calls = <MethodCall>[];
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  tearDown(() {
    ViewerBridge.setKeyboardShortcutHandler(null);
    messenger.setMockMethodCallHandler(channel, null);
    calls.clear();
  });

  test('sends validated shortcut bindings to native code', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return null;
    });
    const RemoteShortcutBinding binding = RemoteShortcutBinding(
      action: RemoteShortcutAction.toggleStats,
      keyUsage: 0x3a,
      modifiers: 1,
      keyLabel: 'F1',
    );

    await ViewerBridge.setKeyboardShortcuts(const [binding]);

    expect(calls.single.method, 'setKeyboardShortcuts');
    expect(calls.single.arguments, <Object>[
      <String, Object>{
        'action': 'toggleStats',
        'keyUsage': 0x3a,
        'modifiers': 1,
      },
    ]);
  });

  test(
    'enables native shortcuts independently of remote key capture',
    () async {
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return true;
      });

      expect(await ViewerBridge.setKeyboardShortcutCapture(true), isTrue);
      expect(calls.single.method, 'setKeyboardShortcutCapture');
      expect(calls.single.arguments, isTrue);
    },
  );

  test('routes native shortcut actions through the method handler', () async {
    RemoteShortcutAction? received;
    ViewerBridge.setKeyboardShortcutHandler((action) => received = action);
    final ByteData message = const StandardMethodCodec().encodeMethodCall(
      const MethodCall('keyboardShortcut', 'toggleFullscreen'),
    );
    final Completer<void> handled = Completer<void>();

    messenger.handlePlatformMessage('chast/viewer', message, (_) {
      handled.complete();
    });
    await handled.future;

    expect(received, RemoteShortcutAction.toggleFullscreen);
  });
}
