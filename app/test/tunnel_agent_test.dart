import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/tunnel_agent.dart';

void main() {
  test('decodes ready and path events and ignores other output', () {
    final TunnelEvent? ready = TunnelEvent.decode(
      '{"event":"ready","listen":"127.0.0.1:45678","host":"devbox",'
      '"path":"direct","remote":"203.0.113.7:13434"}',
    );
    expect(ready, isNotNull);
    expect(ready!.event, 'ready');
    expect(ready.listen, '127.0.0.1:45678');
    expect(ready.host, 'devbox');
    expect(ready.path, 'direct');
    expect(ready.remote, '203.0.113.7:13434');

    expect(TunnelEvent.decode('{"event":"path","path":"relay"}')?.path, 'relay');
    expect(TunnelEvent.decode('time=... level=INFO msg=online'), isNull);
    expect(TunnelEvent.decode('{"event":"other"}'), isNull);
    expect(TunnelEvent.decode('{broken'), isNull);
  });

  test('parses the loopback port from the listen field', () {
    expect(TunnelAgent.parseListenPort('127.0.0.1:41234'), 41234);
    expect(TunnelAgent.parseListenPort('[::1]:41234'), 41234);
    expect(TunnelAgent.parseListenPort('127.0.0.1'), isNull);
    expect(TunnelAgent.parseListenPort('127.0.0.1:'), isNull);
    expect(TunnelAgent.parseListenPort(''), isNull);
  });

  test('builds viewer arguments from the server settings', () {
    expect(
      TunnelAgent.arguments(
        server: 'https://chast.example',
        hostId: 'devbox',
        mode: TunnelMode.relay,
      ),
      <String>[
        'viewer',
        '--server',
        'https://chast.example',
        '--id',
        'devbox',
        '--listen',
        '127.0.0.1:0',
        '--mode',
        'relay',
      ],
    );
  });

  test('a missing executable is reported as a tunnel failure', () async {
    await expectLater(
      TunnelAgent.start(
        server: const ServerSettings(url: 'http://127.0.0.1:1'),
        hostId: 'devbox',
        executable: 'chast-tunnel-does-not-exist.exe',
        argumentOverride: const <String>['viewer'],
      ),
      throwsA(isA<TunnelException>()
          .having((TunnelException e) => e.message, 'message', contains('无法启动隧道代理'))),
    );
  });

  test('starts a child agent and adopts its ready event', () async {
    final File script = await _bat(
      '@echo {"event":"ready","listen":"127.0.0.1:45678","path":"relay"}\r\n',
    );
    final TunnelAgent agent = await _start(script);
    addTearDown(agent.stop);

    expect(agent.host, '127.0.0.1');
    expect(agent.port, 45678);
    expect(agent.path, 'relay');
    expect(agent.pathLabel, '中继');
  }, skip: !Platform.isWindows);

  test('a failed agent carries its exit code and stderr', () async {
    final File script = await _bat(
      '@echo lookup failed 1>&2\r\n@exit /b 3\r\n',
    );
    await expectLater(
      _start(script),
      throwsA(isA<TunnelException>().having(
        (TunnelException e) => e.message,
        'message',
        allOf(contains('退出码 3'), contains('lookup failed')),
      )),
    );
  }, skip: !Platform.isWindows);

  test('an invalid ready address fails the start', () async {
    final File script = await _bat('@echo {"event":"ready","listen":"nonsense"}\r\n');
    await expectLater(
      _start(script),
      throwsA(isA<TunnelException>()
          .having((TunnelException e) => e.message, 'message', contains('无效监听地址'))),
    );
  }, skip: !Platform.isWindows);
}

Future<File> _bat(String contents) async {
  final Directory dir = await Directory.systemTemp.createTemp('chast-tunnel-');
  addTearDown(() => dir.delete(recursive: true));
  final File script = File('${dir.path}${Platform.pathSeparator}agent.bat');
  await script.writeAsString(contents);
  return script;
}

Future<TunnelAgent> _start(File script) {
  return TunnelAgent.start(
    server: const ServerSettings(url: 'http://127.0.0.1:1'),
    hostId: 'devbox',
    executable: 'cmd',
    argumentOverride: <String>['/c', script.path],
    timeout: const Duration(seconds: 10),
  );
}
