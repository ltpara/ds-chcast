import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/address_book_sync.dart';
import 'package:chast_viewer/device_store.dart';
import 'package:chast_viewer/server_api.dart';

void main() {
  const String server = 'http://127.0.0.1:8443';
  final DateTime now = DateTime.utc(2026, 10, 25, 5);

  test('server peers are added, named and promoted', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'devbox',
        server: server,
        saved: true,
        addedAt: now.subtract(const Duration(days: 2)),
      ),
      Device(
        id: 'lab',
        server: server,
        addedAt: now.subtract(const Duration(days: 1)),
        lastUsed: now.subtract(const Duration(hours: 2)),
      ),
    ];
    final List<AddressBookPeer> peers = <AddressBookPeer>[
      const AddressBookPeer(id: 'devbox', name: '办公室'),
      const AddressBookPeer(id: 'lab'),
      const AddressBookPeer(id: 'studio', name: '工作室', tags: <String>['lab']),
    ];

    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: peers,
      serverUrl: server,
      now: now,
    );

    expect(plan.added, 1);
    expect(plan.updated, 2);
    expect(plan.pushes, isEmpty);
    expect(plan.devices, hasLength(3));
    final Device devbox =
        plan.devices.firstWhere((Device item) => item.id == 'devbox');
    expect(devbox.name, '办公室');
    expect(devbox.saved, isTrue);
    final Device lab =
        plan.devices.firstWhere((Device item) => item.id == 'lab');
    expect(lab.saved, isTrue);
    expect(lab.lastUsed, isNotNull);
    final Device studio =
        plan.devices.firstWhere((Device item) => item.id == 'studio');
    expect(studio.name, '工作室');
    expect(studio.server, server);
    expect(studio.saved, isTrue);
    expect(studio.tags, <String>['lab']);
  });

  test('groups adopt from the server, local groups win and stale ids drop', () {
    final List<Device> devices = <Device>[
      Device(id: 'devbox', server: server, saved: true, addedAt: now),
      Device(
        id: 'lab',
        server: server,
        groupId: 'lab-group',
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'office',
        server: server,
        groupId: 'deleted-group',
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'new',
        server: server,
        groupId: 'work',
        saved: true,
        addedAt: now,
      ),
    ];
    final List<AddressBookGroup> groups = <AddressBookGroup>[
      const AddressBookGroup(id: 'work', name: 'Work'),
      const AddressBookGroup(id: 'lab-group', name: 'Lab'),
    ];
    final List<AddressBookPeer> peers = <AddressBookPeer>[
      const AddressBookPeer(id: 'devbox', groupId: 'work'),
      const AddressBookPeer(id: 'lab', groupId: 'work'),
      const AddressBookPeer(id: 'office'),
      const AddressBookPeer(id: 'fresh', groupId: 'work'),
    ];

    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: peers,
      groups: groups,
      serverUrl: server,
      now: now,
    );

    Device byId(String id) =>
        plan.devices.firstWhere((Device item) => item.id == id);
    expect(byId('devbox').groupId, 'work');
    expect(byId('lab').groupId, 'lab-group');
    expect(byId('office').groupId, '');
    expect(byId('new').groupId, 'work');
    expect(byId('fresh').groupId, 'work');
    expect(plan.added, 1);

    final AddressBookPush labPush =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'lab');
    expect(labPush.groupId, 'lab-group');
    expect(labPush.name, isNull);
    final AddressBookPush newPush =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'new');
    expect(newPush.groupId, 'work');
    expect(newPush.name, '');
    expect(plan.pushes.any((AddressBookPush push) => push.id == 'office'),
        isFalse);
    expect(plan.pushes.any((AddressBookPush push) => push.id == 'devbox'),
        isFalse);
  });

  test('local-only peers upload their live group and drop deleted ones', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'kept',
        server: server,
        groupId: 'work',
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'stale',
        server: server,
        groupId: 'gone',
        saved: true,
        addedAt: now,
      ),
    ];
    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[],
      groups: const <AddressBookGroup>[
        AddressBookGroup(id: 'work', name: 'Work'),
      ],
      serverUrl: server,
      now: now,
    );
    final Device stale =
        plan.devices.firstWhere((Device item) => item.id == 'stale');
    expect(stale.groupId, '');
    final AddressBookPush kept =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'kept');
    expect(kept.groupId, 'work');
    final AddressBookPush dropped =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'stale');
    expect(dropped.groupId, isNull);
  });

  test('group merges replace on a full read and apply changes incrementally',
      () {
    const List<AddressBookGroup> current = <AddressBookGroup>[
      AddressBookGroup(id: 'work', name: 'Work'),
      AddressBookGroup(id: 'lab', name: 'Lab'),
      AddressBookGroup(id: 'old', name: 'Old'),
    ];
    const AddressBookSnapshot full = AddressBookSnapshot(
      revision: 9,
      peers: <AddressBookPeer>[],
      groups: <AddressBookGroup>[AddressBookGroup(id: 'work', name: 'Work')],
    );
    expect(mergeAddressBookGroups(current: current, snapshot: full, since: 0),
        <AddressBookGroup>[const AddressBookGroup(id: 'work', name: 'Work')]);
    expect(
      mergeAddressBookGroups(current: current, snapshot: full, since: 12),
      full.groups,
    );

    const AddressBookSnapshot incremental = AddressBookSnapshot(
      revision: 10,
      peers: <AddressBookPeer>[],
      groupChanges: <AddressBookGroup>[
        AddressBookGroup(id: 'lab', name: 'Labor', revision: 10),
        AddressBookGroup(id: 'old', deleted: true, revision: 10),
        AddressBookGroup(id: 'new', name: 'New', revision: 10),
      ],
    );
    final List<AddressBookGroup> merged = mergeAddressBookGroups(
      current: current,
      snapshot: incremental,
      since: 9,
    );
    expect(
      merged.map((AddressBookGroup group) => '${group.id}/${group.name}'),
      <String>['lab/Labor', 'new/New', 'work/Work'],
    );
    expect(merged.any((AddressBookGroup group) => group.id == 'old'), isFalse);
  });

  test('tags adopt from the server, and local tags win and are uploaded', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'devbox',
        server: server,
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'lab',
        server: server,
        tags: const <String>['vip'],
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'office',
        server: server,
        tags: const <String>['office'],
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'new',
        server: server,
        tags: const <String>['fresh'],
        saved: true,
        addedAt: now,
      ),
    ];
    final List<AddressBookPeer> peers = <AddressBookPeer>[
      const AddressBookPeer(id: 'devbox', tags: <String>['office']),
      const AddressBookPeer(id: 'lab', tags: <String>['lab']),
      const AddressBookPeer(id: 'office', tags: <String>['office']),
    ];

    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: peers,
      serverUrl: server,
      now: now,
    );

    final Device devbox =
        plan.devices.firstWhere((Device item) => item.id == 'devbox');
    expect(devbox.tags, <String>['office']);
    final Device lab =
        plan.devices.firstWhere((Device item) => item.id == 'lab');
    expect(lab.tags, <String>['vip']);

    expect(plan.updated, 1);
    expect(plan.added, 0);
    expect(plan.pushes, hasLength(2));
    final AddressBookPush labPush =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'lab');
    expect(labPush.name, isNull);
    expect(labPush.tags, <String>['vip']);
    final AddressBookPush newPush =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'new');
    expect(newPush.name, '');
    expect(newPush.tags, <String>['fresh']);
  });

  test('local names win and local-only entries are uploaded', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'office',
        name: '本机名',
        server: server,
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'lab',
        name: '本地实验室',
        server: server,
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'remote',
        name: '其他服务',
        server: 'http://127.0.0.1:9443',
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'studio',
        server: server,
        addedAt: now,
        lastUsed: now,
      ),
    ];
    final List<AddressBookPeer> peers = <AddressBookPeer>[
      const AddressBookPeer(id: 'office', name: '服务器名'),
      const AddressBookPeer(id: 'studio', name: '服务器工作室'),
    ];

    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: peers,
      serverUrl: server,
      now: now,
    );

    final Set<String> pushed = <String>{
      for (final AddressBookPush push in plan.pushes) push.id,
    };
    expect(pushed, <String>{'office', 'lab'});
    expect(
      plan.pushes.firstWhere((AddressBookPush push) => push.id == 'office').name,
      '本机名',
    );
    expect(
      plan.pushes.firstWhere((AddressBookPush push) => push.id == 'lab').name,
      '本地实验室',
    );
    final Device office =
        plan.devices.firstWhere((Device item) => item.id == 'office');
    expect(office.name, '本机名');
    final Device studio =
        plan.devices.firstWhere((Device item) => item.id == 'studio');
    expect(studio.name, '服务器工作室');
    expect(studio.saved, isTrue);
    expect(plan.updated, 1);
    expect(plan.added, 0);
    expect(
      plan.devices.where((Device item) => item.id == 'remote').single.server,
      'http://127.0.0.1:9443',
    );
  });

  test('a repeated sync is a no-op', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'devbox',
        name: '办公室',
        server: server,
        saved: true,
        addedAt: now,
      ),
    ];
    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[
        AddressBookPeer(id: 'devbox', name: '办公室'),
      ],
      serverUrl: server,
      now: now,
    );
    expect(plan.added, 0);
    expect(plan.updated, 0);
    expect(plan.removed, 0);
    expect(plan.pushes, isEmpty);
    expect(plan.devices, hasLength(1));
  });

  test('tombstones remove saved peers and stop their re-upload', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'devbox',
        name: '办公室',
        server: server,
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'studio',
        server: server,
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'recent',
        server: server,
        addedAt: now,
        lastUsed: now,
      ),
      Device(
        id: 'remote',
        server: 'http://127.0.0.1:9443',
        saved: true,
        addedAt: now,
      ),
    ];

    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[
        AddressBookPeer(id: 'lab', name: '实验室'),
      ],
      tombstones: const <AddressBookPeer>[
        AddressBookPeer(id: 'devbox', deleted: true, revision: 4),
        AddressBookPeer(id: 'recent', deleted: true, revision: 5),
        AddressBookPeer(id: 'remote', deleted: true, revision: 6),
      ],
      serverUrl: server,
      now: now,
    );

    expect(plan.removed, 1);
    expect(plan.added, 1);
    expect(
      plan.pushes.map((AddressBookPush push) => push.id),
      <String>['studio'],
    );
    expect(
      plan.devices.map((Device device) => device.id).toSet(),
      <String>{'studio', 'recent', 'remote', 'lab'},
    );
  });

  test('a pending local deletion suppresses an alive server peer', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'lab',
        name: '实验室',
        server: server,
        saved: true,
        addedAt: now,
      ),
    ];
    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[
        AddressBookPeer(id: 'lab', name: '实验室', revision: 9),
      ],
      pendingDeletions: const <String>{'lab'},
      serverUrl: server,
      now: now,
    );

    expect(plan.removed, 1);
    expect(plan.added, 0);
    expect(plan.pushes, isEmpty);
    expect(plan.devices, isEmpty);
  });

  test('an alive peer wins over a stale tombstone for the same id', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'devbox',
        name: '办公室',
        server: server,
        saved: true,
        addedAt: now,
      ),
    ];
    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[
        AddressBookPeer(id: 'devbox', name: '办公室', revision: 8),
      ],
      tombstones: const <AddressBookPeer>[
        AddressBookPeer(id: 'devbox', deleted: true, revision: 7),
      ],
      serverUrl: server,
      now: now,
    );

    expect(plan.removed, 0);
    expect(plan.devices.single.id, 'devbox');
  });

  test('offline tag and group clears are pushed instead of refilled', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'devbox',
        server: server,
        tagsCleared: true,
        groupCleared: true,
        saved: true,
        addedAt: now,
      ),
    ];
    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[
        AddressBookPeer(
          id: 'devbox',
          tags: <String>['office'],
          groupId: 'work',
          revision: 4,
        ),
      ],
      groups: const <AddressBookGroup>[
        AddressBookGroup(id: 'work', name: 'Work', revision: 3),
      ],
      serverUrl: server,
      now: now,
    );

    final Device devbox = plan.devices.single;
    expect(devbox.tags, isEmpty);
    expect(devbox.groupId, '');
    expect(devbox.tagsCleared, isTrue);
    expect(devbox.groupCleared, isTrue);
    expect(plan.updated, 0);
    expect(plan.pushes, hasLength(1));
    final AddressBookPush push = plan.pushes.single;
    expect(push.name, isNull);
    expect(push.tags, isEmpty);
    expect(push.groupId, '');
  });

  test('a clear whose server row matches settles without a push', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'devbox',
        server: server,
        tagsCleared: true,
        groupCleared: true,
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'lab',
        server: server,
        tags: const <String>['lab'],
        tagsCleared: true,
        groupId: 'work',
        groupCleared: true,
        saved: true,
        addedAt: now,
      ),
    ];
    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[
        AddressBookPeer(id: 'devbox'),
        AddressBookPeer(id: 'lab', tags: <String>['lab'], groupId: 'work'),
      ],
      groups: const <AddressBookGroup>[
        AddressBookGroup(id: 'work', name: 'Work', revision: 3),
      ],
      serverUrl: server,
      now: now,
    );

    final Device devbox =
        plan.devices.firstWhere((Device item) => item.id == 'devbox');
    expect(devbox.tagsCleared, isFalse);
    expect(devbox.groupCleared, isFalse);
    final Device lab =
        plan.devices.firstWhere((Device item) => item.id == 'lab');
    expect(lab.tagsCleared, isFalse);
    expect(lab.groupCleared, isFalse);
    expect(plan.pushes, isEmpty);
  });

  test('peer revisions refresh on pull and travel with uploads', () {
    final List<Device> devices = <Device>[
      Device(
        id: 'sync',
        server: server,
        name: 'Sync',
        tags: const <String>['vip'],
        saved: true,
        revision: 3,
        addedAt: now,
      ),
      Device(
        id: 'lab',
        server: server,
        tags: const <String>['vip'],
        saved: true,
        revision: 1,
        addedAt: now,
      ),
      Device(id: 'local', server: server, saved: true, addedAt: now),
    ];
    final AddressBookPlan plan = planAddressBookSync(
      devices: devices,
      peers: const <AddressBookPeer>[
        AddressBookPeer(id: 'sync', name: 'Sync', tags: <String>['vip'], revision: 5),
        AddressBookPeer(id: 'lab', name: '实验室', tags: <String>['old'], revision: 9),
      ],
      serverUrl: server,
      now: now,
    );

    final Device sync =
        plan.devices.firstWhere((Device item) => item.id == 'sync');
    expect(sync.revision, 5);
    final AddressBookPush labPush =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'lab');
    expect(labPush.tags, <String>['vip']);
    expect(labPush.revision, 9);
    final AddressBookPush localPush =
        plan.pushes.firstWhere((AddressBookPush push) => push.id == 'local');
    expect(localPush.revision, 0);
  });
}
