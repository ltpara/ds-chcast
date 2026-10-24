import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('a status without a transport is UDP', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{});
    expect(status.transport, 'quic');
    expect(status.transportLabel, 'UDP');
    expect(status.transportError, '');
  });

  test('carrier paths map to their overlay labels and keep the reasons', () {
    final ViewerStatus tcp = ViewerStatus.fromMap(<Object?, Object?>{
      'transport': 'tcp',
      'transportError': 'udp: connect: timeout',
    });
    expect(tcp.transportLabel, 'TCP 回退');
    expect(tcp.transportError, 'udp: connect: timeout');

    final ViewerStatus ws = ViewerStatus.fromMap(<Object?, Object?>{
      'transport': 'ws',
      'transportError': 'udp: connect: timeout; tcp: carrier: refused',
    });
    expect(ws.transportLabel, 'WebSocket 回退');
    expect(ws.transportError, contains('tcp: carrier: refused'));

    expect(ViewerStatus.fromMap(<Object?, Object?>{'transport': 'quic'}).transportLabel,
        'UDP');
  });
}
