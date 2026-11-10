import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/tunnel_agent.dart';

void main() {
  for (final bool retryable in <bool>[true, false]) {
    test('startup error preserves retryable=$retryable before exit', () async {
      final File script = await _bat(
        '@echo {"event":"error","error":"lookup failed","retryable":$retryable}\r\n'
        '@exit /b 1\r\n',
      );
      await expectLater(
        _start(script),
        throwsA(isA<TunnelException>()
            .having((TunnelException e) => e.message, 'message', 'lookup failed')
            .having((TunnelException e) => e.retryable, 'retryable', retryable)),
      );
    }, skip: !Platform.isWindows);
  }

  test('unknown startup errors do not opt into retries', () {
    expect(TunnelEvent.decode('{"event":"error","error":"denied"}')!.retryable,
        isFalse);
    expect(const TunnelException('identity changed').retryable, isFalse);
  });

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
    expect(
      TunnelEvent.decode(
        '{"event":"path","path":"relay","relay":"203.0.113.5:41007"}',
      )?.relay,
      '203.0.113.5:41007',
    );
    expect(
      TunnelEvent.decode(
        '{"event":"ready","listen":"127.0.0.1:45678","relay":"[::1]:41007"}',
      )?.relay,
      '[::1]:41007',
    );
    expect(
      TunnelEvent.decode(
        '{"event":"path","path":"relay","carrier":"203.0.113.5:8445",'
        '"carrier_token":"0123456789abcdef"}',
      )?.carrierToken,
      '0123456789abcdef',
    );
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
    expect(
      TunnelAgent.arguments(
        server: 'https://chast.example',
        hostId: 'devbox',
        mode: TunnelMode.relay,
        key: 'dev-secret',
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
        '--server-key-stdin',
      ],
    );
    expect(
      TunnelAgent.arguments(
        server: 'https://chast.example',
        hostId: 'devbox',
        mode: TunnelMode.relay,
        key: 'dev-secret',
        serverPublicKey: 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8',
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
        '--server-key-stdin',
        '--server-pubkey',
        'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8',
      ],
    );
  });

  test('the server key reaches the child over stdin, not argv', () async {
    final File script = await _bat(
      '@set /p KEY=\r\n'
      '@echo {"event":"ready","listen":"127.0.0.1:45678","path":"direct",'
      '"remote":"%KEY%"}\r\n',
    );
    final TunnelAgent agent = await TunnelAgent.start(
      server: const ServerSettings(
        url: 'http://127.0.0.1:1',
        key: 'dev-secret',
      ),
      hostId: 'devbox',
      executable: 'cmd',
      argumentOverride: <String>['/c', script.path],
      timeout: const Duration(seconds: 10),
    );
    addTearDown(agent.stop);

    expect(agent.remote, 'dev-secret');
  }, skip: !Platform.isWindows);

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
      '@echo {"event":"ready","listen":"127.0.0.1:45678","path":"relay",'
      '"relay":"203.0.113.5:41007","carrier":"[2001:db8::5]:8445",'
      '"carrier_token":"secret-token"}\r\n',
    );
    final TunnelAgent agent = await _start(script);
    addTearDown(agent.stop);

    expect(agent.host, '127.0.0.1');
    expect(agent.port, 45678);
    expect(agent.path, 'relay');
    expect(agent.pathLabel, '中继');
    expect(agent.relay, '203.0.113.5:41007');
    expect(agent.relayHost, '203.0.113.5');
    expect(agent.relayPort, 41007);
    expect(agent.carrier, '[2001:db8::5]:8445');
    expect(agent.carrierHost, '2001:db8::5');
    expect(agent.carrierPort, 8445);
    expect(agent.carrierToken, 'secret-token');
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

  test('the proxy URL is requested over stdin, never argv', () {
    final List<String> withProxy = TunnelAgent.arguments(
      server: 'https://chast.example',
      hostId: 'devbox',
      mode: TunnelMode.auto,
      key: 'dev-secret',
      proxy: 'socks5://alice:hunter2@127.0.0.1:10808',
    );
    expect(withProxy, contains('--server-key-stdin'));
    expect(withProxy, contains('--proxy-stdin'));
    expect(withProxy.join(' '), isNot(contains('hunter2')));

    final List<String> proxyOnly = TunnelAgent.arguments(
      server: 'https://chast.example',
      hostId: 'devbox',
      mode: TunnelMode.auto,
      proxy: 'socks5://127.0.0.1:10808',
    );
    expect(proxyOnly, <String>[
      'viewer',
      '--server',
      'https://chast.example',
      '--id',
      'devbox',
      '--listen',
      '127.0.0.1:0',
      '--mode',
      'auto',
      '--proxy-stdin',
    ]);
  });

  test('the account token is requested over stdin, never argv', () {
    final List<String> args = TunnelAgent.arguments(
      server: 'https://chast.example',
      hostId: 'devbox',
      mode: TunnelMode.auto,
      key: 'dev-secret',
      accountToken: 'chast_session_secret',
      proxy: 'socks5://127.0.0.1:10808',
    );
    expect(args, contains('--server-key-stdin'));
    expect(args, contains('--account-token-stdin'));
    expect(args, contains('--proxy-stdin'));
    expect(args.join(' '), isNot(contains('chast_session_secret')));
    expect(
      args.indexOf('--account-token-stdin'),
      lessThan(args.indexOf('--proxy-stdin')),
    );
  });

  test('decodes emergency tickets from ready and renewal events', () {
    final TunnelEvent? ready = TunnelEvent.decode(
      '{"event":"ready","listen":"127.0.0.1:45678",'
      '"access_ticket":"ticket-one","access_expires_at_ms":1700000000000,'
      '"access_subject":"emergency:rootadmin","access_permissions":6}',
    );
    expect(ready?.accessTicket, 'ticket-one');
    expect(ready?.accessExpiresAtMs, 1700000000000);
    expect(ready?.accessSubject, 'emergency:rootadmin');
    expect(ready?.accessPermissions, 6);

    final TunnelEvent? renewal = TunnelEvent.decode(
      '{"event":"access_ticket","access_ticket":"ticket-two",'
      '"access_expires_at_ms":1700000060000,'
      '"access_subject":"emergency:rootadmin"}',
    );
    expect(renewal?.event, 'access_ticket');
    expect(renewal?.accessTicket, 'ticket-two');
    expect(TunnelEvent.decode('{"event":"access_ticket"}')?.accessTicket, '');
  });

  test('the emergency override rides on stdin flags, never argv', () {
    final List<String> args = TunnelAgent.arguments(
      server: 'https://chast.example',
      hostId: 'devbox',
      mode: TunnelMode.auto,
      key: 'dev-secret',
      emergency: const EmergencyAccessRequest(
        adminToken: 'admin-session-secret',
        reason: 'incident 4711',
        clientPublicKey: 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8',
        permissions: 6,
      ),
    );
    expect(args, contains('--emergency-token-stdin'));
    expect(args, contains('--emergency-reason-stdin'));
    expect(args.join(' '), isNot(contains('admin-session-secret')));
    expect(args.join(' '), isNot(contains('incident 4711')));
    expect(
      args[args.indexOf('--emergency-client-key') + 1],
      'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8',
    );
    expect(args[args.indexOf('--emergency-permissions') + 1], '6');

    expect(
      TunnelAgent.arguments(
        server: 'https://chast.example',
        hostId: 'devbox',
        mode: TunnelMode.auto,
        emergency: const EmergencyAccessRequest(
          adminToken: '',
          reason: 'incident',
          clientPublicKey: 'client-key',
        ),
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
        'auto',
      ],
    );
  });

  test(
    'the emergency credentials reach the child in order over stdin',
    () async {
      final File script = await _ps1(
        '\$key = [Console]::In.ReadLine()\r\n'
        '\$account = [Console]::In.ReadLine()\r\n'
        '\$admin = [Console]::In.ReadLine()\r\n'
        '\$reason = [Console]::In.ReadLine()\r\n'
        '\$proxy = [Console]::In.ReadLine()\r\n'
        '[Console]::Out.WriteLine(\'{"event":"ready","listen":"127.0.0.1:45678","remote":"\' + \$key + \',\' + \$account + \',\' + \$admin + \',\' + \$reason + \',\' + \$proxy + \'"}\')\r\n',
      );
      final TunnelAgent agent = await TunnelAgent.start(
        server: const ServerSettings(
          url: 'http://127.0.0.1:1',
          key: 'dev-secret',
          proxy: 'socks5://127.0.0.1:10808',
        ),
        hostId: 'devbox',
        accountToken: 'chast_session_secret',
        emergency: const EmergencyAccessRequest(
          adminToken: 'admin-session-secret',
          reason: 'incident 4711',
          clientPublicKey: 'client-key',
        ),
        executable: 'powershell',
        argumentOverride: <String>[
          '-NoProfile',
          '-NonInteractive',
          '-ExecutionPolicy',
          'Bypass',
          '-File',
          script.path,
        ],
        timeout: const Duration(seconds: 20),
      );
      addTearDown(agent.stop);

      expect(
        agent.remote,
        'dev-secret,chast_session_secret,admin-session-secret,'
        'incident 4711,socks5://127.0.0.1:10808',
      );
    },
    skip: !Platform.isWindows,
  );

  test('adopts the initial emergency ticket and follows renewals', () async {
    final File script = await _bat(
      '@echo {"event":"ready","listen":"127.0.0.1:45678","path":"direct",'
      '"access_ticket":"ticket-one","access_expires_at_ms":1700000000000}\r\n'
      '@ping -n 2 127.0.0.1 >nul\r\n'
      '@echo {"event":"access_ticket","access_ticket":"ticket-two",'
      '"access_expires_at_ms":1700000060000}\r\n'
      '@ping -n 30 127.0.0.1 >nul\r\n',
    );
    final TunnelAgent agent = await _start(script);
    addTearDown(agent.stop);

    expect(agent.accessTicket, 'ticket-one');
    expect(agent.accessExpiresAtMs, 1700000000000);
    final TunnelAccessTicket renewal = await agent.accessTickets.first
        .timeout(const Duration(seconds: 10));
    expect(renewal.ticket, 'ticket-two');
    expect(renewal.expiresAtMs, 1700000060000);
    expect(agent.accessTicket, 'ticket-two');
  }, skip: !Platform.isWindows);

  test(
    'the key, account token and proxy reach the child in that order over stdin',
    () async {
      final File script = await _ps1(
        '\$key = [Console]::In.ReadLine()\r\n'
        '\$token = [Console]::In.ReadLine()\r\n'
        '\$proxy = [Console]::In.ReadLine()\r\n'
        '[Console]::Out.WriteLine(\'{"event":"ready","listen":"127.0.0.1:45678","remote":"\' + \$key + \',\' + \$token + \',\' + \$proxy + \'"}\')\r\n',
      );
      final TunnelAgent agent = await TunnelAgent.start(
        server: const ServerSettings(
          url: 'http://127.0.0.1:1',
          key: 'dev-secret',
          proxy: 'socks5://127.0.0.1:10808',
        ),
        hostId: 'devbox',
        accountToken: 'chast_session_secret',
        executable: 'powershell',
        argumentOverride: <String>[
          '-NoProfile',
          '-NonInteractive',
          '-ExecutionPolicy',
          'Bypass',
          '-File',
          script.path,
        ],
        timeout: const Duration(seconds: 20),
      );
      addTearDown(agent.stop);

      expect(
        agent.remote,
        'dev-secret,chast_session_secret,socks5://127.0.0.1:10808',
      );
    },
    skip: !Platform.isWindows,
  );
}

Future<File> _bat(String contents) async {
  final Directory dir = await Directory.systemTemp.createTemp('chast-tunnel-');
  addTearDown(() => dir.delete(recursive: true));
  final File script = File('${dir.path}${Platform.pathSeparator}agent.bat');
  await script.writeAsString(contents);
  return script;
}

Future<File> _ps1(String contents) async {
  final Directory dir = await Directory.systemTemp.createTemp('chast-tunnel-');
  addTearDown(() => dir.delete(recursive: true));
  final File script = File('${dir.path}${Platform.pathSeparator}agent.ps1');
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
