import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/device_store.dart';

void main() {
  late Directory dir;
  late File file;
  late DeviceStore store;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp('chast-devices-');
    file = File('${dir.path}${Platform.pathSeparator}devices.json');
    store = DeviceStore(file: file);
  });

  tearDown(() async {
    await dir.delete(recursive: true);
  });

  test('a saved device survives a reload with its name', () async {
    await store.save(Device(
      id: 'devbox',
      name: '办公室',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(1),
    ));

    final List<Device> loaded = await store.load();
    expect(loaded, hasLength(1));
    expect(loaded.single.saved, isTrue);
    expect(loaded.single.displayName, '办公室');
    expect(loaded.single.viaServer, isTrue);
    expect(loaded.single.key, 'http://127.0.0.1:8443|devbox');
    expect(loaded.single.trustKey, 'server:http://127.0.0.1:8443/devbox');
  });

  test('touch adds a recent entry, then keeps the saved flag and name', () async {
    await store.save(Device(
      id: 'devbox',
      name: '办公室',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    await store.touch(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));

    final Device device = (await store.load()).single;
    expect(device.saved, isTrue);
    expect(device.name, '办公室');
    expect(device.lastUsed, isNotNull);
  });

  test('recent entries are capped and the oldest are dropped', () async {
    for (int i = 0; i < DeviceStore.maxRecent + 2; ++i) {
      await store.touch(Device(
        id: 'host-$i',
        addedAt: DateTime.fromMillisecondsSinceEpoch(0),
      ));
      await Future<void>.delayed(const Duration(milliseconds: 2));
    }

    final List<Device> loaded = await store.load();
    expect(loaded, hasLength(DeviceStore.maxRecent));
    expect(loaded.map((Device device) => device.id), contains('host-9'));
    expect(loaded.map((Device device) => device.id), isNot(contains('host-0')));
  });

  test('clearRecent keeps the address book', () async {
    await store.save(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    await store.touch(Device(
      id: '10.0.0.2:4433',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));

    await store.clearRecent();
    final List<Device> loaded = await store.load();
    expect(loaded, hasLength(1));
    expect(loaded.single.id, 'devbox');
    expect(loaded.single.saved, isTrue);
  });

  test('promote, rename and remove act on one key', () async {
    await store.touch(Device(
      id: 'laptop',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    const String key = 'direct|laptop';

    await store.promote(key);
    await store.rename(key, '  笔记本  ');
    expect((await store.load()).single.saved, isTrue);
    expect((await store.load()).single.name, '笔记本');

    await store.remove(key);
    expect(await store.load(), isEmpty);
  });

  test('a device with an unusable server is skipped, not made direct', () async {
    await file.writeAsString(jsonEncode(<Object?>[
      <String, Object?>{'id': 'devbox', 'server': 'ftp://chast.example'},
      <String, Object?>{'id': '10.0.0.2:4433'},
      <String, Object?>{'id': '  '},
    ]));

    final List<Device> loaded = await store.load();
    expect(loaded, hasLength(1));
    expect(loaded.single.id, '10.0.0.2:4433');
    expect(loaded.single.viaServer, isFalse);
  });

  test('a corrupt file loads as an empty address book', () async {
    await file.writeAsString('[{"id": ');
    expect(await store.load(), isEmpty);
  });
}
