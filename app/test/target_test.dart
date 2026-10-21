import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/target.dart';

void main() {
  const ServerSettings configured = ServerSettings(
    url: 'http://chast.example:8443',
    mode: TunnelMode.relay,
  );

  test('a bare id goes through the configured service', () {
    final ConnectTarget? target = parseConnectInput('devbox', token: ' t ', server: configured);
    expect(target, isNotNull);
    expect(target!.viaServer, isTrue);
    expect(target.server, 'http://chast.example:8443');
    expect(target.hostId, 'devbox');
    expect(target.mode, TunnelMode.relay);
    expect(target.token, 't');
    expect(target.label, 'devbox');
    expect(target.trustKey, 'server:http://chast.example:8443/devbox');
  });

  test('an explicit address is direct even with a service configured', () {
    final ConnectTarget? target =
        parseConnectInput('10.0.0.2:5555', token: '', server: configured);
    expect(target, isNotNull);
    expect(target!.viaServer, isFalse);
    expect(target.host, '10.0.0.2');
    expect(target.port, 5555);
    expect(target.trustKey, '10.0.0.2:5555');
  });

  test('without a service, a bare name keeps the old direct default', () {
    final ConnectTarget? target =
        parseConnectInput('devbox', token: '', server: const ServerSettings());
    expect(target, isNotNull);
    expect(target!.viaServer, isFalse);
    expect(target.host, 'devbox');
    expect(target.port, 4433);
  });

  test('IPv6 literals keep their brackets in the label', () {
    final ConnectTarget? target = parseConnectTarget('[::1]:4433');
    expect(target, isNotNull);
    expect(target!.host, '::1');
    expect(target.port, 4433);
    expect(target.label, '[::1]:4433');
  });

  test('host ids follow the registry rule', () {
    expect(isValidHostId('devbox'), isTrue);
    expect(isValidHostId('office-pc_1.local'), isTrue);
    expect(isValidHostId('-bad'), isFalse);
    expect(isValidHostId('has space'), isFalse);
    expect(isValidHostId(''), isFalse);
    expect(isValidHostId('a' * 65), isFalse);
  });

  test('a display name decorates the label but not the trust key', () {
    const ConnectTarget target = ConnectTarget(
      '',
      0,
      server: 'http://chast.example',
      hostId: 'devbox',
      displayName: '办公室',
    );
    expect(target.label, '办公室（devbox）');
    expect(target.trustKey, 'server:http://chast.example/devbox');
  });
}
