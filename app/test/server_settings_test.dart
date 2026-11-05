import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_settings.dart';

String _protect(String plaintext) => base64Encode(utf8.encode(plaintext));

String _unprotect(String blob) => utf8.decode(base64Decode(blob));

void main() {
  test('normalizes bare, trailing-slash and pathed URLs', () {
    expect(normalizeServerUrl('203.0.113.7:8443'), 'https://203.0.113.7:8443');
    expect(normalizeServerUrl('chast.example'), 'https://chast.example');
    expect(normalizeServerUrl(' https://chast.example/ '), 'https://chast.example');
    expect(normalizeServerUrl('https://chast.example/api///'), 'https://chast.example/api');
    expect(normalizeServerUrl('http://127.0.0.1:8443/api///'), 'http://127.0.0.1:8443/api');
    expect(normalizeServerUrl('127.0.0.1:8443'), 'http://127.0.0.1:8443');
    expect(normalizeServerUrl('localhost:8443'), 'http://localhost:8443');
    expect(normalizeServerUrl('http://[::1]:8443'), 'http://[::1]:8443');
  });

  test('plain HTTP is only accepted on loopback', () {
    expect(normalizeServerUrl('http://chast.example'), isEmpty);
    expect(normalizeServerUrl('https://chast.example'), 'https://chast.example');
    expect(normalizeServerUrl('ftp://chast.example'), isEmpty);
    expect(normalizeServerUrl('http://'), isEmpty);
    expect(normalizeServerUrl('http://chast.example/?a=1'), isEmpty);
    expect(normalizeServerUrl('http://chast.example/#top'), isEmpty);
    expect(normalizeServerUrl('http://user:pass@127.0.0.1:8443'), isEmpty);
    expect(normalizeServerUrl('https://user:pass@chast.example'), isEmpty);
  });

  test('proxy URLs require an explicit supported scheme and port', () {
    expect(normalizeProxyUrl(''), isEmpty);
    expect(
      normalizeProxyUrl('socks5://127.0.0.1:10808'),
      'socks5://127.0.0.1:10808',
    );
    expect(
      normalizeProxyUrl('SOCKS5H://user:pass@proxy.example:1080/'),
      'socks5h://user:pass@proxy.example:1080',
    );
    expect(
      normalizeProxyUrl('http://proxy.example:8080'),
      'http://proxy.example:8080',
    );
    expect(
      normalizeProxyUrl('https://proxy.example:8443'),
      'https://proxy.example:8443',
    );
    expect(normalizeProxyUrl('socks5://[::1]:1080'), 'socks5://[::1]:1080');

    expect(normalizeProxyUrl('127.0.0.1:10808'), isEmpty);
    expect(normalizeProxyUrl('ftp://proxy.example:21'), isEmpty);
    expect(normalizeProxyUrl('socks5://proxy.example'), isEmpty);
    expect(normalizeProxyUrl('socks5://proxy.example:0'), isEmpty);
    expect(normalizeProxyUrl('socks5://proxy.example:70000'), isEmpty);
    expect(normalizeProxyUrl('socks5://proxy.example:1080/path'), isEmpty);
    expect(normalizeProxyUrl('socks5://proxy.example:1080?x=1'), isEmpty);
    expect(normalizeProxyUrl('socks5://proxy.example:1080#frag'), isEmpty);
  });

  test('an invalid configured proxy fails closed', () {
    expect(
      effectiveProxyUrl('socks5://127.0.0.1:10808'),
      'socks5://127.0.0.1:10808',
    );
    expect(
      () => effectiveProxyUrl('ftp://proxy.example:21'),
      throwsFormatException,
    );
    expect(effectiveProxyUrl(''), isEmpty);
  });

  test('unknown modes fall back to auto', () {
    expect(TunnelModeName.parse('relay'), TunnelMode.relay);
    expect(TunnelModeName.parse('direct'), TunnelMode.direct);
    expect(TunnelModeName.parse('bogus'), TunnelMode.auto);
    expect(TunnelModeName.parse(null), TunnelMode.auto);
  });

  test('settings round-trip through the store', () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-settings-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}server.json');
    final ServerSettingsStore store = ServerSettingsStore(file: file);

    expect((await store.load()).configured, isFalse);

    await store.save(const ServerSettings(
      url: 'https://chast.example/',
      mode: TunnelMode.relay,
      key: 'dev-secret',
      proxy: 'socks5://127.0.0.1:10808',
      uploadRecordings: true,
      useWebSocket: true,
    ));
    final ServerSettings loaded = await store.load();
    expect(loaded.url, 'https://chast.example');
    expect(loaded.mode, TunnelMode.relay);
    expect(loaded.key, 'dev-secret');
    expect(loaded.proxy, 'socks5://127.0.0.1:10808');
    expect(loaded.uploadRecordings, isTrue);
    expect(loaded.useWebSocket, isTrue);
    expect(loaded.configured, isTrue);
  });

  test('the server key is protected at rest', () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-settings-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}server.json');
    final ServerSettingsStore store = ServerSettingsStore(
      file: file,
      protectSecret: (String plaintext) async => _protect(plaintext),
      unprotectSecret: (String blob) async => _unprotect(blob),
    );

    await store.save(const ServerSettings(
      url: 'https://chast.example',
      mode: TunnelMode.relay,
      key: 'dev-secret',
      uploadRecordings: true,
    ));

    final String text = await file.readAsString();
    expect(text, contains('"protected"'));
    expect(text, isNot(contains('dev-secret')));

    final ServerSettings loaded = await store.load();
    expect(loaded.url, 'https://chast.example');
    expect(loaded.mode, TunnelMode.relay);
    expect(loaded.key, 'dev-secret');
    expect(loaded.uploadRecordings, isTrue);
  });

  test('proxy credentials are protected at rest', () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-settings-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}server.json');
    final ServerSettingsStore store = ServerSettingsStore(
      file: file,
      protectSecret: (String plaintext) async => _protect(plaintext),
      unprotectSecret: (String blob) async => _unprotect(blob),
    );

    await store.save(const ServerSettings(
      url: 'https://chast.example',
      proxy: 'socks5://alice:hunter2@127.0.0.1:10808',
    ));

    final String text = await file.readAsString();
    expect(text, contains('"protected"'));
    expect(text, isNot(contains('hunter2')));

    final ServerSettings loaded = await store.load();
    expect(loaded.proxy, 'socks5://alice:hunter2@127.0.0.1:10808');
  });

  test('a proxy-only legacy file is migrated too', () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-settings-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}server.json');
    await file.writeAsString(jsonEncode(<String, Object?>{
      'url': 'https://chast.example',
      'proxy': 'socks5://alice:hunter2@127.0.0.1:10808',
    }));

    final ServerSettings loaded = await ServerSettingsStore(
      file: file,
      protectSecret: (String plaintext) async => _protect(plaintext),
      unprotectSecret: (String blob) async => _unprotect(blob),
    ).load();

    expect(loaded.proxy, 'socks5://alice:hunter2@127.0.0.1:10808');
    final String text = await file.readAsString();
    expect(text, contains('"protected"'));
    expect(text, isNot(contains('hunter2')));
  });

  test('a legacy plaintext key is migrated to a protected envelope', () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-settings-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}server.json');
    await file.writeAsString(jsonEncode(<String, Object?>{
      'url': 'https://chast.example',
      'mode': 'direct',
      'key': 'dev-secret',
      'upload_recordings': false,
    }));

    final ServerSettings loaded = await ServerSettingsStore(
      file: file,
      protectSecret: (String plaintext) async => _protect(plaintext),
      unprotectSecret: (String blob) async => _unprotect(blob),
    ).load();

    expect(loaded.key, 'dev-secret');
    final String text = await file.readAsString();
    expect(text, contains('"protected"'));
    expect(text, isNot(contains('dev-secret')));
  });

  test('a failing protector never writes the key in the clear', () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-settings-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}server.json');
    final ServerSettingsStore store = ServerSettingsStore(
      file: file,
      protectSecret: (String _) async => throw StateError('dpapi'),
      unprotectSecret: (String blob) async => _unprotect(blob),
    );

    await store.save(const ServerSettings(
      url: 'https://chast.example',
      key: 'dev-secret',
      proxy: 'socks5://alice:hunter2@127.0.0.1:10808',
    ));

    final String text = await file.readAsString();
    expect(text, isNot(contains('dev-secret')));
    expect(text, isNot(contains('hunter2')));
    final ServerSettings loaded = await store.load();
    expect(loaded.url, 'https://chast.example');
    expect(loaded.key, isEmpty);
    expect(loaded.proxy, isEmpty);
  });

  test('a corrupt file loads as unconfigured', () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-settings-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}${Platform.pathSeparator}server.json');
    await file.writeAsString('{not json');

    final ServerSettings loaded = await ServerSettingsStore(file: file).load();
    expect(loaded.configured, isFalse);
    expect(loaded.mode, TunnelMode.auto);
    expect(loaded.key, isEmpty);
  });
}
