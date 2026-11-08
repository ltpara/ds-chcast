import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/display_stream_controller.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('chast/viewer');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  late List<String> events;
  late List<Map<String, Object>> streams;
  late int request, focused;
  late String refusal;
  late DisplayStreamController controller;
  Map<String, Object> stream(int id, int index) => {
    'streamId': id,
    'displayIndex': index,
    'width': 640,
    'height': 480,
    'sourceWidth': 1280,
    'sourceHeight': 960,
    'originX': index == 0 ? -1280 : 0,
    'originY': -100,
    'published': 1,
    'bound': true,
    'ready': true,
    'error': '',
  };
  setUp(() {
    events = [];
    streams = [stream(7, 0), stream(9, 1)];
    request = 0;
    focused = 7;
    refusal = '';
    int next = 100;
    controller = DisplayStreamController(
      register: () async {
        events.add('register:$next');
        return next++;
      },
      output: (id) async => id * 100,
      unregister: (id) async {
        events.add('unregister:$id');
      },
    );
    messenger.setMockMethodCallHandler(channel, (call) async {
      switch (call.method) {
        case 'displayStreams':
          return {
            'active': true,
            'requestId': request,
            'results': 1,
            'focused': focused,
            'error': refusal,
            'streams': streams,
          };
        case 'takeDisplayCursors':
          return [];
        case 'captureDisplays':
          request = (call.arguments as Map)['requestId'] as int;
          events.add('select:${(call.arguments as Map)['set']}');
          return true;
        case 'bindDisplayOutput':
          final args = call.arguments as Map;
          events.add('bind:${args['streamId']}:${args['output']}');
          return true;
        case 'focusDisplay':
          focused = call.arguments as int;
          return true;
      }
      throw StateError(call.method);
    });
  });
  tearDown(() async {
    await controller.close();
    controller.dispose();
    messenger.setMockMethodCallHandler(channel, null);
  });

  test(
    'retained stream keeps texture; retired stream detaches before release',
    () async {
      await controller.refresh();
      expect(controller.textures.map((t) => t.textureId), [100, 101]);
      expect(controller.selected!.stream.streamId, 7);
      await controller.focus(9);
      expect(controller.selected!.stream.streamId, 9);
      streams = [stream(9, 0)];
      await controller.refresh();
      expect(controller.textures.single.textureId, 101);
      expect(controller.textures.single.stream.displayIndex, 0);
      expect(
        events.indexOf('bind:7:0'),
        lessThan(events.indexOf('unregister:100')),
      );
      streams.add(stream(11, 1));
      await controller.refresh();
      expect(controller.textures.map((t) => t.textureId), [101, 102]);
    },
  );

  test('empty set leaves controller available for resubscription', () async {
    await controller.refresh();
    expect(await controller.select([]), isTrue);
    expect(controller.busy, isTrue);
    expect(await controller.select([1]), isFalse);
    streams = [];
    focused = -1;
    await controller.refresh();
    expect(controller.busy, isFalse);
    expect(controller.textures, isEmpty);
    expect(controller.selected, isNull);
    expect(controller.state.active, isTrue);
    expect(await controller.select([1]), isTrue);
    streams = [stream(11, 1)];
    focused = 11;
    await controller.refresh();
    expect(controller.selected!.textureId, 102);
    expect(events.where((e) => e.startsWith('select:')), [
      'select:[]',
      'select:[1]',
    ]);
  });

  test('refused operation preserves all working outputs', () async {
    await controller.refresh();
    await controller.select([123]);
    refusal = 'no such display';
    await controller.refresh();
    expect(controller.error, refusal);
    expect(controller.textures.map((t) => t.textureId), [100, 101]);
    expect(events.where((e) => e.startsWith('unregister:')), isEmpty);
  });

  test(
    'registration completing after close never binds into another session',
    () async {
      await controller.close();
      controller.dispose();
      final registration = Completer<int?>();
      final started = Completer<void>();
      controller = DisplayStreamController(
        register: () {
          started.complete();
          return registration.future;
        },
        output: (id) async {
          events.add('output:$id');
          return 123;
        },
        unregister: (id) async {
          events.add('unregister:$id');
        },
      );
      final refresh = controller.refresh();
      await started.future;
      final close = controller.close();
      registration.complete(55);
      await refresh;
      await close;
      expect(events, ['unregister:55']);
      expect(await controller.select([0]), isFalse);
    },
  );

  test(
    'detach error retains ownership until native stop makes release safe',
    () async {
      await controller.refresh();
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => throw PlatformException(code: 'closed'),
      );
      await expectLater(controller.close(), throwsA(isA<PlatformException>()));
      expect(events.where((e) => e.startsWith('unregister:')), isEmpty);
      await controller.close(sessionStopped: true);
      expect(events.where((e) => e.startsWith('unregister:')), [
        'unregister:100',
        'unregister:101',
      ]);
    },
  );
}
