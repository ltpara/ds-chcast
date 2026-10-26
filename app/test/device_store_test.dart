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

  test('tags survive a reload, are de-duplicated and can be cleared', () async {
    await store.save(Device(
      id: 'devbox',
      name: '办公室',
      server: 'http://127.0.0.1:8443',
      tags: const <String>['office', ' office ', '', 'vip'],
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    final Device saved = (await store.load()).single;
    expect(saved.tags, <String>['office', 'vip']);

    await store.updateTags(saved.key, const <String>['lab', 'vip']);
    expect((await store.load()).single.tags, <String>['lab', 'vip']);

    await store.updateTags(saved.key, const <String>[]);
    expect((await store.load()).single.tags, isEmpty);
  });

  test('touch keeps the tags of an existing saved entry', () async {
    await store.save(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      tags: const <String>['office'],
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    await store.touch(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    expect((await store.load()).single.tags, <String>['office']);
  });

  test('a MAC is normalized, survives reload/touch/save and can be cleared',
      () async {
    await store.save(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      mac: 'AA:BB:CC:DD:EE:FF',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    expect((await store.load()).single.mac, 'AA:BB:CC:DD:EE:FF');

    await store.touch(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    await store.save(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    Device saved = (await store.load()).single;
    expect(saved.mac, 'AA:BB:CC:DD:EE:FF');

    await store.updateMac(saved.key, 'aa-bb-cc-dd-ee-01');
    saved = (await store.load()).single;
    expect(saved.mac, 'AA:BB:CC:DD:EE:01');

    await store.updateMac(saved.key, 'not-a-mac');
    expect((await store.load()).single.mac, isEmpty);

    await file.writeAsString(jsonEncode(<Object?>[
      <String, Object?>{
        'id': 'lab',
        'server': 'http://127.0.0.1:8443',
        'mac': 'aa-bb-cc-dd-ee-02',
        'addedAt': 0,
      },
      <String, Object?>{
        'id': 'bad',
        'server': 'http://127.0.0.1:8443',
        'mac': 'nope',
        'addedAt': 0,
      },
    ]));
    final List<Device> loaded = await store.load();
    expect(loaded.firstWhere((Device item) => item.id == 'lab').mac,
        'AA:BB:CC:DD:EE:02');
    expect(loaded.firstWhere((Device item) => item.id == 'bad').mac, isEmpty);
  });

  test('peer revision survives reload, touch and save', () async {
    const String server = 'http://127.0.0.1:8443';
    await store.save(Device(
      id: 'devbox',
      name: '办公室',
      server: server,
      revision: 7,
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    expect((await store.load()).single.revision, 7);

    await store.touch(Device(
      id: 'devbox',
      server: server,
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    expect((await store.load()).single.revision, 7);

    await store.save(Device(
      id: 'devbox',
      server: server,
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    expect((await store.load()).single.revision, 7);

    final String key = 'http://127.0.0.1:8443|devbox';
    await store.setPeerRevision(key, 9);
    expect((await store.load()).single.revision, 9);

    await store.setPeerRevision(key, 0);
    expect((await store.load()).single.revision, 9);
  });

  test('a negative stored revision loads as unknown', () async {
    await file.writeAsString(jsonEncode(<Object?>[
      <String, Object?>{
        'id': 'devbox',
        'server': 'http://127.0.0.1:8443',
        'saved': true,
        'revision': -3,
        'addedAt': 0,
      },
    ]));
    expect((await store.load()).single.revision, 0);
  });

  test('groups survive a reload, survive touch and can be changed', () async {
    await store.save(Device(
      id: 'devbox',
      name: '办公室',
      server: 'http://127.0.0.1:8443',
      groupId: 'work',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    Device saved = (await store.load()).single;
    expect(saved.groupId, 'work');

    await store.touch(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    saved = (await store.load()).single;
    expect(saved.groupId, 'work');

    await store.updateGroup(saved.key, ' lab ');
    expect((await store.load()).single.groupId, 'lab');

    await store.updateGroup(saved.key, '');
    expect((await store.load()).single.groupId, '');
  });

  test('clearing tags or a group records an offline intent until settled',
      () async {
    await store.save(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      tags: const <String>['office'],
      groupId: 'work',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    final String key = (await store.load()).single.key;

    await store.updateTags(key, const <String>[]);
    Device device = (await store.load()).single;
    expect(device.tagsCleared, isTrue);
    expect(device.groupCleared, isFalse);

    await store.updateGroup(key, '');
    device = (await store.load()).single;
    expect(device.groupCleared, isTrue);

    await store.touch(Device(
      id: 'devbox',
      server: 'http://127.0.0.1:8443',
      addedAt: DateTime.fromMillisecondsSinceEpoch(0),
    ));
    device = (await store.load()).single;
    expect(device.tagsCleared, isTrue);
    expect(device.groupCleared, isTrue);

    await store.markPushSettled(key, tags: true);
    device = (await store.load()).single;
    expect(device.tagsCleared, isFalse);
    expect(device.groupCleared, isTrue);

    await store.markPushSettled(key, group: true);
    device = (await store.load()).single;
    expect(device.groupCleared, isFalse);

    await store.updateTags(key, const <String>['lab']);
    expect((await store.load()).single.tagsCleared, isFalse);
  });

  test('a legacy device without a group loads as ungrouped', () async {
    await file.parent.create(recursive: true);
    await file.writeAsString(jsonEncode(<Object?>[
      <String, Object?>{
        'id': 'oldbox',
        'name': '旧设备',
        'server': 'http://127.0.0.1:8443',
        'tags': <String>['lab'],
        'saved': true,
        'addedAt': 1,
      },
    ]));
    final Device device = (await store.load()).single;
    expect(device.groupId, '');
    expect(device.tags, <String>['lab']);
    expect(device.tagsCleared, isFalse);
    expect(device.groupCleared, isFalse);
  });

  test('pending deletions persist per account and can be removed', () async {
    const String server = 'http://127.0.0.1:8443';
    final PendingDeletion first = PendingDeletion(
      server: server,
      username: 'alice',
      id: 'lab',
      deletedAt: DateTime.fromMillisecondsSinceEpoch(10),
    );
    await store.queueDeletion(first);
    await store.queueDeletion(first);
    await store.queueDeletion(PendingDeletion(
      server: server,
      username: 'bob',
      id: 'lab',
      deletedAt: DateTime.fromMillisecondsSinceEpoch(20),
    ));

    final List<PendingDeletion> queued = await store.loadPendingDeletions();
    expect(queued, hasLength(2));
    expect(queued.first.key, 'http://127.0.0.1:8443|alice|lab');
    expect(queued.last.username, 'bob');

    await store.removePendingDeletion(first.key);
    final List<PendingDeletion> left = await store.loadPendingDeletions();
    expect(left, hasLength(1));
    expect(left.single.username, 'bob');
  });

  test('a corrupt deletion outbox loads as empty', () async {
    await File(
      '${dir.path}${Platform.pathSeparator}pending_deletions.json',
    ).writeAsString('not json');
    expect(await store.loadPendingDeletions(), isEmpty);
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
