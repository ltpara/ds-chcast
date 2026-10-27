import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_identity.dart';
import 'package:chast_viewer/server_settings.dart';

const String _keyA = 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8';
const String _keyAFingerprint =
    '630DCD2966C4336691125448BBB25B4FF412A49C732DB2C8ABC1B8581BD710DD';
// Same length, different content.
const String _keyB = 'Hx4dHBsaGRgXFhUUExIREA8ODQwLCgkIBwYFBAMCAQA';

void main() {
  test('parses the key and derives the SHA-256 fingerprint', () {
    final ServerIdentity? identity = ServerIdentity.parse(' $_keyA ');
    expect(identity, isNotNull);
    expect(identity!.publicKey, _keyA);
    expect(identity.fingerprint, _keyAFingerprint);

    final ServerIdentity? padded = ServerIdentity.parse('$_keyA=');
    expect(padded?.publicKey, _keyA);
    expect(padded?.fingerprint, _keyAFingerprint);
  });

  test('rejects keys that are not 32 base64url bytes', () {
    expect(ServerIdentity.parse(''), isNull);
    expect(ServerIdentity.parse('AAAA'), isNull);
    expect(ServerIdentity.parse('not base64!'), isNull);
  });

  test('formats a fingerprint for comparison', () {
    expect(
      formatServerFingerprint(_keyAFingerprint),
      '630DCD29 66C43366 91125448 BBB25B4F F412A49C 732DB2C8 ABC1B858 1BD710DD',
    );
    expect(shortenServerFingerprint(_keyAFingerprint), '630DCD29…1BD710DD');
    expect(shortenServerFingerprint('ABC'), 'ABC');
  });

  test('pins by normalised URL, rotates explicitly and audits every step',
      () async {
    final Directory dir = Directory.systemTemp.createTempSync('chast-ident');
    addTearDown(() => dir.deleteSync(recursive: true));
    final ServerIdentityStore store = ServerIdentityStore(
      file: File('${dir.path}\\pins.json'),
      auditFile: File('${dir.path}\\audit.jsonl'),
    );
    expect(await store.pinFor('http://127.0.0.1:8443'), isNull);

    final ServerIdentity first = ServerIdentity.parse(_keyA)!;
    final ServerIdentity second = ServerIdentity.parse(_keyB)!;
    await store.pin('http://127.0.0.1:8443/', first);
    expect(
      (await store.pinFor('http://127.0.0.1:8443'))?.identity.fingerprint,
      _keyAFingerprint,
    );
    expect((await store.pinFor('http://127.0.0.1:8444')), isNull);

    final ServerIdentityPin? old = await store.pinFor('http://127.0.0.1:8443');
    await store.pin('http://127.0.0.1:8443', second, previous: old);
    expect(
      (await store.pinFor('http://127.0.0.1:8443'))?.identity.publicKey,
      second.publicKey,
    );

    final List<String> lines =
        (await File('${dir.path}\\audit.jsonl').readAsString())
            .trim()
            .split('\n');
    expect(lines.length, 2);
    final Map<String, Object?> firstRecord =
        jsonDecode(lines[0]) as Map<String, Object?>;
    final Map<String, Object?> secondRecord =
        jsonDecode(lines[1]) as Map<String, Object?>;
    expect(firstRecord['action'], 'tofu');
    expect(firstRecord['server'], 'http://127.0.0.1:8443');
    expect(firstRecord['fingerprint'], _keyAFingerprint);
    expect(firstRecord['previous_fingerprint'], isNull);
    expect(secondRecord['action'], 'rotate');
    expect(secondRecord['fingerprint'], second.fingerprint);
    expect(secondRecord['previous_fingerprint'], _keyAFingerprint);

    await store.forget('http://127.0.0.1:8443');
    expect(await store.pinFor('http://127.0.0.1:8443'), isNull);
    final List<String> all =
        (await File('${dir.path}\\audit.jsonl').readAsString())
            .trim()
            .split('\n');
    expect(all.length, 3);
    expect((jsonDecode(all[2]) as Map<String, Object?>)['action'], 'forget');
  });

  test('ignores a pin whose stored key is invalid', () async {
    final Directory dir = Directory.systemTemp.createTempSync('chast-ident');
    addTearDown(() => dir.deleteSync(recursive: true));
    final File file = File('${dir.path}\\pins.json');
    await file.writeAsString(jsonEncode(<String, Object?>{
      'http://127.0.0.1:8443': <String, Object?>{
        'public_key': 'broken',
        'fingerprint': _keyAFingerprint,
      },
    }));
    final ServerIdentityStore store = ServerIdentityStore(
      file: file,
      auditFile: File('${dir.path}\\audit.jsonl'),
    );
    expect(await store.pinFor('http://127.0.0.1:8443'), isNull);
  });

  test('the settings store normalises the same URL the pin does', () {
    expect(
      normalizeServerUrl('http://127.0.0.1:8443/'),
      'http://127.0.0.1:8443',
    );
  });
}
