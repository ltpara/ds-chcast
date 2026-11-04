import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/tunnel_proxy.dart';

void main() {
  test('the bridge is a loopback HTTP proxy fed over stdin', () {
    expect(ServerProxyBridge.arguments(), <String>[
      'proxy',
      '--proxy-stdin',
      '--listen',
      '127.0.0.1:0',
    ]);
  });

  test('an invalid upstream is rejected before a child starts', () {
    expect(
      () => ServerProxyBridge.ensure('ftp://proxy.example:21'),
      throwsA(
        isA<ServerProxyException>().having(
          (ServerProxyException e) => e.message,
          'message',
          contains('出站代理地址无效'),
        ),
      ),
    );
    expect(
      () => ServerProxyBridge.ensure(''),
      throwsA(isA<ServerProxyException>()),
    );
  });
}
