import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';

void main() {
  late Directory temp;
  late File file;

  setUp(() async {
    temp = await Directory.systemTemp.createTemp('chast-account-');
    file = File('${temp.path}/account.json');
  });

  tearDown(() async {
    await temp.delete(recursive: true);
  });

  test('missing file has no session', () async {
    final AccountSessionStore store = AccountSessionStore(file: file);
    expect(await store.load(), isNull);
  });

  test('session round-trips through the file', () async {
    final AccountSessionStore store = AccountSessionStore(file: file);
    final DateTime expires = DateTime.utc(2027, 1, 2, 3, 4, 5);
    final DateTime synced = DateTime.utc(2026, 12, 1, 2, 3, 4);
    await store.save(AccountSession(
      server: 'http://127.0.0.1:8443',
      username: 'alice',
      token: 'token-value',
      expiresAt: expires,
      syncedAt: synced,
      bookRevision: 12,
    ));

    final AccountSession? loaded = await store.load();
    expect(loaded, isNotNull);
    expect(loaded!.server, 'http://127.0.0.1:8443');
    expect(loaded.username, 'alice');
    expect(loaded.token, 'token-value');
    expect(loaded.expiresAt, expires);
    expect(loaded.syncedAt, synced);
    expect(loaded.bookRevision, 12);
    expect(loaded.copyWith(bookRevision: 13).bookRevision, 13);
    expect(loaded.copyWith(bookRevision: 13).syncedAt, synced);
    expect(loaded.isActiveAt(DateTime.utc(2026, 12, 1)), isTrue);
    expect(loaded.isActiveAt(DateTime.utc(2027, 1, 2, 3, 4, 5)), isFalse);
  });

  test('server URL is normalized and malformed sessions are ignored', () async {
    await file.writeAsString(jsonEncode(<String, Object?>{
      'server': '127.0.0.1:8443/',
      'username': ' alice ',
      'token': ' token ',
      'expiresAt': 1800000000000,
    }));
    final AccountSessionStore store = AccountSessionStore(file: file);
    final AccountSession? loaded = await store.load();
    expect(loaded?.server, 'http://127.0.0.1:8443');
    expect(loaded?.username, 'alice');
    expect(loaded?.token, 'token');
    expect(loaded?.syncedAt, isNull);
    expect(loaded?.bookRevision, 0);

    for (final Object? broken in <Object?>[
      <String, Object?>{'username': 'alice', 'token': 't', 'expiresAt': 1},
      <String, Object?>{
        'server': 'http://127.0.0.1:8443',
        'username': '',
        'token': 't',
        'expiresAt': 1,
      },
      <String, Object?>{
        'server': 'http://127.0.0.1:8443',
        'username': 'alice',
        'token': 't',
      },
      <String, Object?>{},
      'not a map',
    ]) {
      await file.writeAsString(jsonEncode(broken));
      expect(await store.load(), isNull, reason: '$broken');
    }
  });

  test('clear removes the persisted session', () async {
    final AccountSessionStore store = AccountSessionStore(file: file);
    await store.save(AccountSession(
      server: 'http://127.0.0.1:8443',
      username: 'alice',
      token: 'token-value',
      expiresAt: DateTime.utc(2027),
    ));
    expect(await file.exists(), isTrue);
    await store.clear();
    expect(await file.exists(), isFalse);
    expect(await store.load(), isNull);
  });
}
