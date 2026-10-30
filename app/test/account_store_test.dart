import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';

Future<String> fakeProtect(String plaintext) async =>
    base64Encode(utf8.encode('sealed:$plaintext'));

Future<String> fakeUnprotect(String blob) async {
  final String text = utf8.decode(base64Decode(blob));
  if (!text.startsWith('sealed:')) {
    throw const FormatException('not a sealed blob');
  }
  return text.substring('sealed:'.length);
}

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

  test('refresh token round-trips and outlives the access token', () async {
    final AccountSessionStore store = AccountSessionStore(file: file);
    final DateTime access = DateTime.utc(2026, 12, 1, 0, 30);
    final DateTime refresh = DateTime.utc(2027, 1, 2);
    await store.save(AccountSession(
      server: 'http://127.0.0.1:8443',
      username: 'alice',
      token: 'access-value',
      expiresAt: access,
      refreshToken: 'refresh-value',
      refreshExpiresAt: refresh,
    ));

    final AccountSession? loaded = await store.load();
    expect(loaded, isNotNull);
    expect(loaded!.refreshToken, 'refresh-value');
    expect(loaded.refreshExpiresAt, refresh);
    expect(loaded.isActiveAt(access.add(const Duration(minutes: 1))), isFalse);
    expect(
      loaded.canRefreshAt(access.add(const Duration(minutes: 1))),
      isTrue,
    );
    expect(
      loaded.isUsableAt(access.add(const Duration(minutes: 1))),
      isTrue,
    );
    expect(loaded.isUsableAt(refresh.add(const Duration(minutes: 1))), isFalse);

    final AccountSession renewed = loaded.copyWith(
      token: 'new-access',
      expiresAt: refresh,
      refreshToken: 'new-refresh',
      refreshExpiresAt: refresh.add(const Duration(days: 30)),
    );
    expect(renewed.token, 'new-access');
    expect(renewed.refreshToken, 'new-refresh');
    expect(renewed.username, 'alice');
  });

  test('legacy sessions without a refresh token stay usable until expiry',
      () async {
    await file.writeAsString(jsonEncode(<String, Object?>{
      'server': 'http://127.0.0.1:8443',
      'username': 'alice',
      'token': 'access-value',
      'expiresAt': DateTime.utc(2027).millisecondsSinceEpoch,
    }));
    final AccountSessionStore store = AccountSessionStore(file: file);
    final AccountSession? loaded = await store.load();
    expect(loaded?.refreshToken, '');
    expect(loaded?.refreshExpiresAt, isNull);
    expect(loaded!.isActiveAt(DateTime.utc(2026, 12)), isTrue);
    expect(loaded.canRefreshAt(DateTime.utc(2026, 12)), isFalse);
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

  test('a protector keeps tokens out of the plaintext file', () async {
    final AccountSessionStore store = AccountSessionStore(
      file: file,
      protectSecret: fakeProtect,
      unprotectSecret: fakeUnprotect,
    );
    await store.save(AccountSession(
      server: 'http://127.0.0.1:8443',
      username: 'alice',
      token: 'token-value',
      expiresAt: DateTime.utc(2027),
      refreshToken: 'refresh-value',
    ));

    final String raw = await file.readAsString();
    expect(raw, isNot(contains('token-value')));
    expect(raw, isNot(contains('refresh-value')));
    final Map<String, Object?> envelope =
        jsonDecode(raw) as Map<String, Object?>;
    expect(envelope['version'], 1);
    expect(envelope['protected'], isA<String>());
    expect(envelope.containsKey('token'), isFalse);

    final AccountSession? loaded = await store.load();
    expect(loaded?.token, 'token-value');
    expect(loaded?.refreshToken, 'refresh-value');
  });

  test('a legacy plaintext file is migrated when loaded with a protector',
      () async {
    await file.writeAsString(jsonEncode(<String, Object?>{
      'server': 'http://127.0.0.1:8443',
      'username': 'alice',
      'token': 'access-value',
      'expiresAt': DateTime.utc(2027).millisecondsSinceEpoch,
    }));
    final AccountSessionStore store = AccountSessionStore(
      file: file,
      protectSecret: fakeProtect,
      unprotectSecret: fakeUnprotect,
    );

    final AccountSession? loaded = await store.load();
    expect(loaded?.token, 'access-value');
    final String raw = await file.readAsString();
    expect(raw, isNot(contains('access-value')));
    expect((jsonDecode(raw) as Map<String, Object?>)['protected'], isA<String>());

    expect((await store.load())?.token, 'access-value');
  });

  test('an envelope this account cannot open has no session', () async {
    await file.writeAsString(jsonEncode(<String, Object?>{
      'version': 1,
      'protected': 'AAAA',
    }));
    final AccountSessionStore throwing = AccountSessionStore(
      file: file,
      unprotectSecret: (String _) async => throw const FormatException('no'),
    );
    expect(await throwing.load(), isNull);

    final AccountSessionStore withoutProtector =
        AccountSessionStore(file: file);
    expect(await withoutProtector.load(), isNull);
  });

  test('a failing protector still persists the session plainly', () async {
    final AccountSessionStore store = AccountSessionStore(
      file: file,
      protectSecret: (String _) async => throw StateError('dpapi'),
    );
    await store.save(AccountSession(
      server: 'http://127.0.0.1:8443',
      username: 'alice',
      token: 'token-value',
      expiresAt: DateTime.utc(2027),
    ));
    expect(await file.readAsString(), contains('token-value'));
    expect((await AccountSessionStore(file: file).load())?.token,
        'token-value');
  });
}
