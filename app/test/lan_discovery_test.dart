import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/lan_discovery.dart';

void main() {
  test('scan parses peers, merges duplicates and prefers the LAN address',
      () async {
    late List<String> captured;
    final LanDiscovery discovery =
        LanDiscovery(runner: (String executable, List<String> arguments) async {
      captured = arguments;
      return ProcessResult(1, 0, '''
[
  {"id":"lanbox","address":"127.0.0.1:21129","addresses":["127.0.0.1:21129"],
   "hostname":"DESKTOP-LAN","username":"admin","platform":"windows/amd64",
   "version":"dev","mac":"18:31:BF:B2:CC:8A"},
  {"id":"lanbox","address":"192.168.8.20:21129",
   "addresses":["192.168.8.20:21129"]},
  {"id":"bad id","address":"10.0.0.9:21129"},
  {"address":"10.0.0.9:21129"}
]
''', '');
    });

    final List<LanPeer> peers = await discovery.scan(
      selfId: 'me',
      timeout: const Duration(milliseconds: 700),
    );

    expect(
      captured,
      <String>[
        'discover',
        '--json',
        '--timeout',
        '700ms',
        '--id',
        'me',
      ],
    );
    expect(peers, hasLength(1));
    final LanPeer peer = peers.single;
    expect(peer.id, 'lanbox');
    expect(peer.address, '192.168.8.20:21129');
    expect(
      peer.addresses,
      <String>['192.168.8.20:21129', '127.0.0.1:21129'],
    );
    expect(peer.hostname, 'DESKTOP-LAN');
    expect(peer.username, 'admin');
    expect(peer.platform, 'windows/amd64');
    expect(peer.mac, '18:31:BF:B2:CC:8A');
  });

  test('empty answers are not an error', () async {
    final LanDiscovery discovery = LanDiscovery(
      runner: (String executable, List<String> arguments) async =>
          ProcessResult(1, 0, '[]', ''),
    );
    expect(await discovery.scan(), isEmpty);
  });

  test('non-zero exit exposes stderr', () async {
    final LanDiscovery discovery = LanDiscovery(
      runner: (String executable, List<String> arguments) async =>
          ProcessResult(1, 2, '', 'bind: address already in use'),
    );
    await expectLater(
      discovery.scan(),
      throwsA(isA<LanDiscoveryException>().having(
        (LanDiscoveryException error) => error.message,
        'message',
        contains('address already in use'),
      )),
    );
  });

  test('invalid JSON is rejected', () async {
    final LanDiscovery discovery = LanDiscovery(
      runner: (String executable, List<String> arguments) async =>
          ProcessResult(1, 0, 'not json', ''),
    );
    await expectLater(
      discovery.scan(),
      throwsA(isA<LanDiscoveryException>()),
    );
  });

  test('peer fields are trimmed, bounded and validated', () {
    expect(LanPeer.fromJson('nope'), isNull);
    expect(LanPeer.fromJson(<String, Object?>{'id': ''}), isNull);
    expect(LanPeer.fromJson(<String, Object?>{'id': ' bad id'}), isNull);

    final LanPeer? peer = LanPeer.fromJson(<String, Object?>{
      'id': ' box ',
      'hostname': '  ${'x' * 300}  ',
      'addresses': <Object?>[' 10.0.0.2:21129 ', 7],
    });
    expect(peer, isNotNull);
    expect(peer!.id, 'box');
    expect(peer.hostname.length, 128);
    expect(peer.address, '10.0.0.2:21129');
    expect(peer.addresses, <String>['10.0.0.2:21129']);
  });
}
