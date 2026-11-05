import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('a status without rules reports none', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{});
    expect(status.portForwardRules, isEmpty);
    expect(status.portForwardEnabled, isFalse);
  });

  test('parallel rules keep their order and counters', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'portForwardEnabled': true,
      'portForwardRules': <Object?>[
        <Object?, Object?>{
          'ruleId': 1,
          'active': true,
          'localPort': 46001,
          'remoteHost': '192.168.1.10',
          'remotePort': 3389,
          'connections': 3,
          'failures': 1,
          'bytesUp': 2048,
          'bytesDown': 4096,
          'error': '',
        },
        <Object?, Object?>{
          'ruleId': 2,
          'active': false,
          'localPort': 46002,
          'remoteHost': 'db.internal',
          'remotePort': 5432,
          'connections': 0,
          'failures': 0,
          'bytesUp': 0,
          'bytesDown': 0,
          'error': 'the host refused the forward target',
        },
      ],
    });
    expect(status.portForwardEnabled, isTrue);
    expect(status.portForwardRules, hasLength(2));
    final ViewerPortForwardRule first = status.portForwardRules[0];
    expect(first.ruleId, 1);
    expect(first.active, isTrue);
    expect(first.localPort, 46001);
    expect(first.remoteHost, '192.168.1.10');
    expect(first.remotePort, 3389);
    expect(first.connections, 3);
    expect(first.failures, 1);
    expect(first.bytesUp, 2048);
    expect(first.bytesDown, 4096);
    expect(first.error, isEmpty);
    final ViewerPortForwardRule second = status.portForwardRules[1];
    expect(second.ruleId, 2);
    expect(second.active, isFalse);
    expect(second.remoteHost, 'db.internal');
    expect(second.error, 'the host refused the forward target');
  });

  test('rules without an id are dropped', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'portForwardRules': <Object?>[
        <Object?, Object?>{'ruleId': 0, 'remoteHost': 'ignored'},
        <Object?, Object?>{'ruleId': 7, 'remoteHost': 'kept'},
      ],
    });
    expect(status.portForwardRules, hasLength(1));
    expect(status.portForwardRules.single.ruleId, 7);
    expect(status.portForwardRules.single.remoteHost, 'kept');
  });
}
