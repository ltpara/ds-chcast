import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_settings.dart';

void main() {
  test('normalizes bare, trailing-slash and pathed URLs', () {
    expect(normalizeServerUrl('203.0.113.7:8443'), 'http://203.0.113.7:8443');
    expect(normalizeServerUrl(' https://chast.example/ '), 'https://chast.example');
    expect(normalizeServerUrl('http://chast.example/api///'), 'http://chast.example/api');
    expect(normalizeServerUrl('http://[::1]:8443'), 'http://[::1]:8443');
  });

  test('rejects values that are not an HTTP base URL', () {
    expect(normalizeServerUrl(''), isEmpty);
    expect(normalizeServerUrl('ftp://chast.example'), isEmpty);
    expect(normalizeServerUrl('http://'), isEmpty);
    expect(normalizeServerUrl('http://chast.example/?a=1'), isEmpty);
    expect(normalizeServerUrl('http://chast.example/#top'), isEmpty);
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
      uploadRecordings: true,
    ));
    final ServerSettings loaded = await store.load();
    expect(loaded.url, 'https://chast.example');
    expect(loaded.mode, TunnelMode.relay);
    expect(loaded.key, 'dev-secret');
    expect(loaded.uploadRecordings, isTrue);
    expect(loaded.configured, isTrue);
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
