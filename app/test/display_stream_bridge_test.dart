import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/display_stream_bridge.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('chast/viewer');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test(
    'subscription lists retain add/sub/set and empty-set semantics',
    () async {
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return true;
      });
      expect(
        await DisplayStreamBridge.capture(
          requestId: 1,
          add: [1],
          sub: [0],
          set: [2],
        ),
        isTrue,
      );
      expect(await DisplayStreamBridge.capture(requestId: 2, sub: [1]), isTrue);
      expect(await DisplayStreamBridge.capture(requestId: 3), isTrue);
      expect(calls[0].arguments, {
        'requestId': 1,
        'add': [1],
        'sub': [0],
        'set': [2],
      });
      expect(calls[1].arguments, {
        'requestId': 2,
        'add': [],
        'sub': [1],
        'set': [],
      });
      expect(calls[2].arguments, {
        'requestId': 3,
        'add': [],
        'sub': [],
        'set': [],
      });
    },
  );

  test(
    'unsigned stream identity is independent of index and texture pointer',
    () async {
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'bindDisplayOutput') {
          expect(call.arguments, {
            'streamId': 0xffffffff,
            'output': 0x123456789,
          });
          return true;
        }
        return {
          'active': true,
          'requestId': 12,
          'results': 2,
          'error': '',
          'focused': 0xffffffff,
          'streams': [
            {
              'streamId': 0xffffffff,
              'displayIndex': 1,
              'width': 640,
              'height': 480,
              'sourceWidth': 1280,
              'sourceHeight': 960,
              'originX': -1280,
              'originY': -100,
              'published': 10,
              'bound': true,
              'ready': true,
              'error': '',
            },
          ],
        };
      });
      expect(await DisplayStreamBridge.bind(0xffffffff, 0x123456789), isTrue);
      final state = await DisplayStreamBridge.subscriptions();
      expect(state.requestId, 12);
      expect(state.streams.single.streamId, 0xffffffff);
      expect(state.streams.single.displayIndex, 1);
      expect(state.streams.single.originX, -1280);
      expect(state.streams.single.sourceWidth, 1280);
      expect(state.focused, 0xffffffff);
      expect(state.streams.single.ready, isTrue);
    },
  );

  test(
    'source cursor retains negative positions and typed sprite bytes',
    () async {
      final sprite = Uint8List.fromList(List<int>.filled(16, 128));
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => [
          {
            'streamId': 9,
            'visible': false,
            'x': -20,
            'y': 10,
            'width': 2,
            'height': 2,
            'hotspotX': 1,
            'hotspotY': 0,
            'shapeId': 0x1fffffffffffff,
            'bgra': sprite,
          },
        ],
      );
      final cursor = (await DisplayStreamBridge.takePointers()).single;
      expect(cursor.streamId, 9);
      expect(cursor.x, -20);
      expect(cursor.shapeId, 0x1fffffffffffff);
      expect(cursor.bgra, sprite);
    },
  );
}
