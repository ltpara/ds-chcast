import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const MethodChannel channel = MethodChannel('chast/viewer');
  final List<MethodCall> calls = <MethodCall>[];

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  void mock(Future<Object?> Function(MethodCall call) handler) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall call) {
          calls.add(call);
          return handler(call);
        });
  }

  test('status carries the session list and select answers', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'inputEnabled': true,
      'permissions': ViewerPermission.input | ViewerPermission.sessionSelect,
      'sessionSelectSupported': true,
      'sessions': <Object?>[
        <Object?, Object?>{
          'id': 1,
          'user': 'admin',
          'active': true,
          'current': true,
        },
        <Object?, Object?>{
          'id': 2,
          'user': 'other',
          'active': true,
          'current': false,
        },
      ],
      'sessionListCode': ViewerSessionListCode.ok.code,
      'sessionListError': '',
      'sessionListUpdates': 4,
      'sessionSelectId': 2,
      'sessionSelectCode': ViewerSessionSelectCode.busy.code,
      'sessionSelectError': 'another viewer is in control',
      'sessionSelectUpdates': 1,
    });
    expect(status.sessionSelectSupported, isTrue);
    expect(status.sessions, hasLength(2));
    expect(status.sessions[0].id, 1);
    expect(status.sessions[0].user, 'admin');
    expect(status.sessions[0].current, isTrue);
    expect(status.sessions[1].label, 'other · Session 2');
    expect(status.sessionListCode, ViewerSessionListCode.ok.code);
    expect(status.sessionListError, isEmpty);
    expect(status.sessionListUpdates, 4);
    expect(status.sessionSelectId, 2);
    expect(status.sessionSelectCode, ViewerSessionSelectCode.busy.code);
    expect(status.sessionSelectError, 'another viewer is in control');
    expect(status.sessionSelectUpdates, 1);
    expect(status.acceptsSessionSelect, isTrue);
  });

  test('session select defaults to unavailable', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.sessionSelectSupported, isFalse);
    expect(status.sessions, isEmpty);
    expect(status.sessionListCode, 0);
    expect(status.sessionListError, isEmpty);
    expect(status.sessionListUpdates, 0);
    expect(status.sessionSelectId, 0);
    expect(status.sessionSelectCode, 0);
    expect(status.sessionSelectError, isEmpty);
    expect(status.sessionSelectUpdates, 0);
    expect(status.acceptsSessionSelect, isFalse);
  });

  test('session select needs the published grant and a live stream', () {
    final ViewerStatus grantedOnly = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'inputEnabled': true,
      'permissions':
          ViewerPermission.input | ViewerPermission.sessionSelect,
    });
    expect(grantedOnly.acceptsSessionSelect, isFalse);

    final ViewerStatus connecting = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'handshaking',
      'inputEnabled': true,
      'permissions':
          ViewerPermission.input | ViewerPermission.sessionSelect,
      'sessionSelectSupported': true,
    });
    expect(connecting.acceptsSessionSelect, isFalse);
  });

  test('session entries drop invalid ids and keep an empty user', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'sessions': <Object?>[
        <Object?, Object?>{'id': 0, 'user': 'zero'},
        <Object?, Object?>{'id': -3, 'user': 'negative'},
        <Object?, Object?>{'id': 0xFFFFFFFF, 'user': 'reserved'},
        <Object?, Object?>{'id': 3},
      ],
    });
    expect(status.sessions, hasLength(1));
    expect(status.sessions.single.id, 3);
    expect(status.sessions.single.user, isEmpty);
    expect(status.sessions.single.label, 'Session 3');
  });

  test('requestSessionList reports the queue', () async {
    calls.clear();
    mock((MethodCall call) async => true);
    expect(await ViewerBridge.requestSessionList(), isTrue);

    calls.clear();
    mock((MethodCall call) async => null);
    expect(await ViewerBridge.requestSessionList(), isFalse);

    expect(calls.single.method, 'requestSessionList');
    expect(calls.single.arguments, isNull);
  });

  test('requestSessionSelect sends the session id', () async {
    calls.clear();
    mock((MethodCall call) async => true);
    expect(await ViewerBridge.requestSessionSelect(7), isTrue);

    calls.clear();
    mock((MethodCall call) async => false);
    expect(await ViewerBridge.requestSessionSelect(2), isFalse);

    expect(calls.single.method, 'requestSessionSelect');
    expect(calls.single.arguments, <String, Object>{'id': 2});
  });
}
