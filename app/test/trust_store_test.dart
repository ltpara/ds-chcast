import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/trust_store.dart';
import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  const String hex = '0123456789ABCDEF0123456789ABCDEF'
      '0123456789ABCDEF0123456789ABCDEF';

  test('normalizes fingerprints copied in any common form', () {
    expect(normalizeFingerprint(hex), hex);
    expect(
      normalizeFingerprint('01:23:45:67:89:ab:cd:ef:'
          '01:23:45:67:89:ab:cd:ef:'
          '01:23:45:67:89:ab:cd:ef:'
          '01:23:45:67:89:ab:cd:ef'),
      hex,
    );
    expect(normalizeFingerprint('  ${hex.substring(0, 32)} ${hex.substring(32)} '), hex);
    expect(normalizeFingerprint(''), '');
    expect(normalizeFingerprint('AB12'), '');
    expect(normalizeFingerprint('G' * 64), '');
    expect(normalizeFingerprint('A' * 63), '');
    expect(normalizeFingerprint('A' * 65), '');
  });

  test('shortens a fingerprint for the overlay without losing its ends', () {
    expect(shortenFingerprint(hex), '01234567…89ABCDEF');
    expect(shortenFingerprint('ABCD'), 'ABCD');
  });

  test('stores a pin per endpoint and ignores malformed files', () async {
    final Directory dir = Directory.systemTemp.createTempSync('chast-trust-');
    addTearDown(() => dir.deleteSync(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}trust.json');
    final TrustStore store = TrustStore(file: file);

    expect(await store.fingerprintFor('192.168.1.10:4433'), '');
    expect(await store.trust('192.168.1.10:4433', '01:23:45:67'), '');
    expect(await store.fingerprintFor('192.168.1.10:4433'), '');

    expect(await store.trust('192.168.1.10:4433', hex.toLowerCase()), hex);
    expect(await store.fingerprintFor('192.168.1.10:4433'), hex);
    expect(await store.fingerprintFor('192.168.1.11:4433'), '');

    final Map<String, Object?> raw = jsonDecode(await file.readAsString()) as Map<String, Object?>;
    expect(raw['192.168.1.10:4433'], hex);

    await store.forget('192.168.1.10:4433');
    expect(await store.fingerprintFor('192.168.1.10:4433'), '');

    await file.writeAsString('{"host:1": "not-a-fingerprint", "host:2": "$hex"}');
    expect(await store.load(), <String, String>{'host:2': hex});

    await file.writeAsString('not json');
    expect(await store.load(), isEmpty);
  });

  test('parses the presented fingerprint from the native status', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'serverFingerprint': hex,
    });
    expect(status.serverFingerprint, hex);

    const ViewerStatus withoutIdentity = ViewerStatus();
    expect(withoutIdentity.serverFingerprint, isEmpty);
  });
}
