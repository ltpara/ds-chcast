import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/device_store.dart';
import 'package:chast_viewer/lan_discovery.dart';
import 'package:chast_viewer/pages/home_page.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_identity.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/trust_store.dart';

class _MemoryDevices extends DeviceStore {
  _MemoryDevices(this.devices, {List<PendingDeletion>? pending})
    : pending = pending ?? <PendingDeletion>[];

  final List<Device> devices;
  final List<PendingDeletion> pending;

  @override
  Future<List<Device>> load() async => List<Device>.of(devices);

  @override
  Future<List<PendingDeletion>> loadPendingDeletions() async =>
      List<PendingDeletion>.of(pending);

  @override
  Future<List<PendingDeletion>> queueDeletion(PendingDeletion deletion) async {
    if (pending.every((PendingDeletion item) => item.key != deletion.key)) {
      pending.add(deletion);
    }
    return List<PendingDeletion>.of(pending);
  }

  @override
  Future<List<PendingDeletion>> removePendingDeletion(String key) async {
    pending.removeWhere((PendingDeletion item) => item.key == key);
    return List<PendingDeletion>.of(pending);
  }

  @override
  Future<List<Device>> replaceAll(List<Device> devices) async {
    this.devices
      ..clear()
      ..addAll(devices);
    return List<Device>.of(this.devices);
  }

  @override
  Future<List<Device>> save(Device device) async {
    final int index = devices.indexWhere(
      (Device item) => item.key == device.key,
    );
    if (index >= 0) {
      devices[index] = device.copyWith(saved: true);
    } else {
      devices.add(device.copyWith(saved: true));
    }
    return List<Device>.of(devices);
  }

  @override
  Future<List<Device>> rename(String key, String name) async {
    final int index = devices.indexWhere((Device device) => device.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(name: name.trim());
    }
    return List<Device>.of(devices);
  }

  @override
  Future<List<Device>> updateTags(String key, List<String> tags) async {
    final int index = devices.indexWhere((Device device) => device.key == key);
    if (index >= 0) {
      final List<String> clean = Device.cleanTags(tags);
      devices[index] = devices[index].copyWith(
        tags: clean,
        tagsCleared: clean.isEmpty,
      );
    }
    return List<Device>.of(devices);
  }

  @override
  Future<List<Device>> updateGroup(String key, String groupId) async {
    final int index = devices.indexWhere((Device device) => device.key == key);
    if (index >= 0) {
      final String value = groupId.trim();
      devices[index] = devices[index].copyWith(
        groupId: value,
        groupCleared: value.isEmpty,
      );
    }
    return List<Device>.of(devices);
  }

  @override
  Future<List<Device>> updateMac(String key, String mac) async {
    final int index = devices.indexWhere((Device device) => device.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(mac: mac);
    }
    return List<Device>.of(devices);
  }

  @override
  Future<List<Device>> markPushSettled(
    String key, {
    bool tags = false,
    bool group = false,
  }) async {
    final int index = devices.indexWhere((Device device) => device.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(
        tagsCleared: tags ? false : null,
        groupCleared: group ? false : null,
      );
    }
    return List<Device>.of(devices);
  }

  @override
  Future<List<Device>> setPeerRevision(String key, int revision) async {
    if (revision > 0) {
      final int index = devices.indexWhere(
        (Device device) => device.key == key,
      );
      if (index >= 0) {
        devices[index] = devices[index].copyWith(revision: revision);
      }
    }
    return List<Device>.of(devices);
  }

  @override
  Future<List<Device>> remove(String key) async {
    devices.removeWhere((Device device) => device.key == key);
    return List<Device>.of(devices);
  }
}

class _MemoryAccount extends AccountSessionStore {
  _MemoryAccount([this.session]);

  AccountSession? session;

  @override
  Future<AccountSession?> load() async => session;

  @override
  Future<void> save(AccountSession value) async => session = value;

  @override
  Future<void> clear() async => session = null;
}

class _MemoryServers extends ServerSettingsStore {
  _MemoryServers(this.settings);

  final ServerSettings settings;

  @override
  Future<ServerSettings> load() async => settings;
}

class _MemoryTrust extends TrustStore {
  _MemoryTrust(this.entries);

  final Map<String, String> entries;

  @override
  Future<Map<String, String>> load() async => Map<String, String>.of(entries);
}

class _MemoryIdentity extends ServerIdentityStore {
  _MemoryIdentity([Map<String, ServerIdentityPin>? pins])
    : pins = pins ?? <String, ServerIdentityPin>{};

  final Map<String, ServerIdentityPin> pins;

  @override
  Future<Map<String, ServerIdentityPin>> load() async =>
      Map<String, ServerIdentityPin>.of(pins);

  @override
  Future<ServerIdentityPin?> pinFor(String serverUrl) async =>
      pins[normalizeServerUrl(serverUrl)];

  @override
  Future<ServerIdentityPin> pin(
    String serverUrl,
    ServerIdentity identity, {
    ServerIdentityPin? previous,
  }) async {
    final ServerIdentityPin record = ServerIdentityPin(
      identity: identity,
      pinnedAt: DateTime.now(),
    );
    pins[normalizeServerUrl(serverUrl)] = record;
    return record;
  }

  @override
  Future<void> forget(String serverUrl) async {
    pins.remove(normalizeServerUrl(serverUrl));
  }
}

class _FakeLanDiscovery extends LanDiscovery {
  const _FakeLanDiscovery(this.peers);

  final List<LanPeer> peers;

  @override
  Future<List<LanPeer>> scan({
    String selfId = '',
    Duration timeout = LanDiscovery.defaultTimeout,
  }) async => peers;
}

String _savedCount(WidgetTester tester) =>
    tester.widget<Text>(find.byKey(const Key('saved-count'))).data ?? '';

void main() {
  testWidgets(
    'home shows the address book, recent entries and service status',
    (WidgetTester tester) async {
      tester.view.physicalSize = const Size(1600, 1000);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);

      final DateTime now = DateTime.now();
      final List<Device> devices = <Device>[
        Device(
          id: 'devbox',
          name: '办公室',
          server: 'http://127.0.0.1:8443',
          saved: true,
          addedAt: now.subtract(const Duration(hours: 1)),
        ),
        Device(id: '10.0.0.2:4433', addedAt: now, lastUsed: now),
      ];

      int lookups = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: HomePage(
            deviceStore: _MemoryDevices(devices),
            serverStore: _MemoryServers(
              const ServerSettings(url: 'http://127.0.0.1:8443'),
            ),
            identityStore: _MemoryIdentity(),
            trustStore: _MemoryTrust(<String, String>{
              'server:http://127.0.0.1:8443/devbox': 'AB' * 32,
            }),
            accountStore: _MemoryAccount(),
            lookupBatch: (ServerSettings server, List<String> hostIds) async {
              lookups++;
              return BatchLookupResult(
                hosts: <String, HostInfo>{
                  for (final String hostId in hostIds)
                    hostId: HostInfo(
                      id: hostId,
                      online: true,
                      candidate: '127.0.0.1:14434',
                      relay: '127.0.0.1:41000',
                    ),
                },
              );
            },
          ),
        ),
      );
      for (int i = 0; i < 4; ++i) {
        await tester.pump(const Duration(milliseconds: 20));
      }

      expect(find.text('我的设备'), findsOneWidget);
      expect(find.text('办公室'), findsOneWidget);
      expect(find.text('最近连接'), findsOneWidget);
      expect(find.text('10.0.0.2:4433'), findsWidgets);
      expect(find.textContaining('身份与中继'), findsNothing);
      expect(find.textContaining('已固定 1 个主机身份'), findsOneWidget);
      expect(find.textContaining('在线'), findsWidgets);
      expect(lookups, greaterThan(0));

      await tester.pumpWidget(const SizedBox());
    },
  );

  testWidgets('LAN scan adds selected peers to the address book', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final List<Device> devices = <Device>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(devices),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async =>
              const BatchLookupResult(hosts: <String, HostInfo>{}),
          lanDiscovery: const _FakeLanDiscovery(<LanPeer>[
            LanPeer(
              id: 'lanbox',
              address: '192.168.8.20:21129',
              addresses: <String>['192.168.8.20:21129', '127.0.0.1:21129'],
              hostname: 'DESKTOP-LAN',
              username: 'admin',
              platform: 'windows/amd64',
              version: 'dev',
              mac: '18:31:BF:B2:CC:8A',
            ),
          ]),
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('lan-scan')));
    await tester.pumpAndSettle();
    expect(find.text('DESKTOP-LAN'), findsOneWidget);
    expect(find.textContaining('lanbox'), findsWidgets);
    expect(find.textContaining('18:31:BF:B2:CC:8A'), findsOneWidget);

    await tester.tap(find.byKey(const Key('lan-add')));
    await tester.pumpAndSettle();
    expect(devices, hasLength(1));
    expect(devices.single.id, 'lanbox');
    expect(devices.single.name, 'DESKTOP-LAN');
    expect(devices.single.server, 'http://127.0.0.1:8443');
    expect(devices.single.mac, '18:31:BF:B2:CC:8A');
    expect(devices.single.saved, isTrue);
    expect(find.text('DESKTOP-LAN'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('address book batches per server and shows alias and tags', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final DateTime now = DateTime.now();
    final List<Device> devices = <Device>[
      Device(
        id: 'studio',
        server: 'http://127.0.0.1:8443',
        saved: true,
        addedAt: now,
      ),
      Device(
        id: 'lab',
        server: 'http://127.0.0.1:9443',
        saved: true,
        addedAt: now,
      ),
    ];
    final List<List<String>> calls = <List<String>>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(devices),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            calls.add(<String>[server.url, ...hostIds]);
            return BatchLookupResult(
              hosts: <String, HostInfo>{
                'studio': const HostInfo(
                  id: 'studio',
                  online: true,
                  alias: '工作室',
                  tags: <String>['office', 'prod'],
                ),
                'lab': const HostInfo(id: 'lab', online: false),
              },
            );
          },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(calls, hasLength(2));
    expect(calls.map((List<String> call) => call.join('|')).toSet(), <String>{
      'http://127.0.0.1:8443|studio',
      'http://127.0.0.1:9443|lab',
    });
    expect(find.text('工作室'), findsOneWidget);
    expect(find.textContaining('#office #prod'), findsOneWidget);
    expect(find.textContaining('离线'), findsWidgets);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('home shows a server update offer until it is dismissed', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'devbox',
              server: 'http://127.0.0.1:8443',
              saved: true,
              addedAt: DateTime.now(),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return BatchLookupResult(
              hosts: <String, HostInfo>{
                for (final String hostId in hostIds)
                  hostId: HostInfo(id: hostId, online: true),
              },
              software: const SoftwareInfo(
                version: '2.1.0',
                url: 'https://downloads.example.com/chast-2.1.0.exe',
              ),
            );
          },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(find.textContaining('可更新到 2.1.0'), findsOneWidget);
    await tester.tap(find.text('下载地址'));
    await tester.pumpAndSettle();
    expect(find.text('发现新版本'), findsOneWidget);
    expect(find.textContaining('chast-2.1.0.exe'), findsOneWidget);
    await tester.tap(find.text('关闭'));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('忽略此版本'));
    await tester.pump();
    expect(find.textContaining('可更新到 2.1.0'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a server alias and tags can be edited from the address book', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    int lookups = 0;
    ServerSettings? updatedServer;
    String? updatedHost;
    String? updatedAlias;
    List<String>? updatedTags;
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'studio',
              server: 'http://127.0.0.1:8443',
              saved: true,
              addedAt: DateTime.now(),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(
              url: 'http://127.0.0.1:8443',
              key: 'dev-secret',
            ),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            lookups++;
            return BatchLookupResult(
              hosts: <String, HostInfo>{
                for (final String hostId in hostIds)
                  hostId: const HostInfo(
                    id: 'studio',
                    online: true,
                    alias: '工作室',
                    tags: <String>['office'],
                  ),
              },
            );
          },
          labelsUpdater:
              (
                ServerSettings server,
                String hostId, {
                required String alias,
                required List<String> tags,
              }) async {
                updatedServer = server;
                updatedHost = hostId;
                updatedAlias = alias;
                updatedTags = tags;
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('服务端别名/标签'));
    await tester.pumpAndSettle();

    expect(find.text('服务端别名与标签'), findsOneWidget);
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('labels-alias')))
          .controller
          ?.text,
      '工作室',
    );
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('labels-tags')))
          .controller
          ?.text,
      'office',
    );

    await tester.enterText(find.byKey(const Key('labels-alias')), ' 设计室 ');
    await tester.enterText(
      find.byKey(const Key('labels-tags')),
      ' office, design, office ， ',
    );
    await tester.tap(find.byKey(const Key('labels-save')));
    await tester.pumpAndSettle();

    expect(updatedServer?.url, 'http://127.0.0.1:8443');
    expect(updatedServer?.key, 'dev-secret');
    expect(updatedHost, 'studio');
    expect(updatedAlias, '设计室');
    expect(updatedTags, <String>['office', 'design']);
    expect(find.text('服务端别名与标签'), findsNothing);
    expect(find.text('服务端别名与标签已保存'), findsOneWidget);
    expect(lookups, greaterThan(1));

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a failed label update keeps the dialog and shows the error', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'studio',
              server: 'http://127.0.0.1:8443',
              saved: true,
              addedAt: DateTime.now(),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return BatchLookupResult(
              hosts: <String, HostInfo>{
                for (final String hostId in hostIds)
                  hostId: HostInfo(id: hostId, online: true),
              },
            );
          },
          labelsUpdater:
              (
                ServerSettings server,
                String hostId, {
                required String alias,
                required List<String> tags,
              }) async {
                throw const ServerApiException('连接服务密钥无效', statusCode: 401);
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('服务端别名/标签'));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('labels-save')));
    await tester.pumpAndSettle();

    expect(find.text('服务端别名与标签'), findsOneWidget);
    expect(find.textContaining('密钥无效'), findsOneWidget);
    expect(find.text('服务端别名与标签已保存'), findsNothing);

    await tester.tap(find.text('取消'));
    await tester.pumpAndSettle();
    expect(find.text('服务端别名与标签'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('account login saves a session and syncs the address book', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount();
    final List<Device> devices = <Device>[
      Device(
        id: 'lab',
        name: '实验室',
        server: 'http://127.0.0.1:8443',
        saved: true,
        addedAt: DateTime.now(),
      ),
    ];
    String? loginUser;
    String? loginPassword;
    final List<String> pushes = <String>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(devices),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return BatchLookupResult(
              hosts: <String, HostInfo>{
                for (final String hostId in hostIds)
                  hostId: HostInfo(id: hostId, online: true),
              },
            );
          },
          accountLogin:
              (
                ServerSettings server, {
                required String username,
                required String password,
                String totpCode = '',
                String recoveryCode = '',
                String smsCode = '',
                String emailCode = '',
              }) async {
                loginUser = username;
                loginPassword = password;
                return const AccountLoginResult(
                  token: 'tok',
                  username: 'alice',
                  expiresInMs: 3600000,
                );
              },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                expect(token, 'tok');
                expect(since, 0);
                return const AddressBookSnapshot(
                  revision: 4,
                  peers: <AddressBookPeer>[
                    AddressBookPeer(id: 'studio', name: '工作室'),
                    AddressBookPeer(id: 'lab', name: 'ServerLab'),
                  ],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add('$peerId|${name ?? ''}');
                return AddressBookPeer(id: peerId);
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(find.text('登录账号'), findsOneWidget);
    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('account-username')),
      ' alice ',
    );
    await tester.enterText(find.byKey(const Key('account-password')), 'secret');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(loginUser, 'alice');
    expect(loginPassword, 'secret');
    expect(accounts.session?.username, 'alice');
    expect(accounts.session?.token, 'tok');
    expect(accounts.session?.syncedAt, isNotNull);
    expect(accounts.session?.bookRevision, 4);
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('实验室'), findsOneWidget);
    expect(pushes, <String>['lab|实验室']);
    expect(find.textContaining('地址簿已同步'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('account login asks for a two-factor code when required', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final List<String> codes = <String>[];
    final _MemoryAccount accounts = _MemoryAccount();
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          accountLogin:
              (
                ServerSettings server, {
                required String username,
                required String password,
                String totpCode = '',
                String recoveryCode = '',
                String smsCode = '',
                String emailCode = '',
              }) async {
                codes.add(totpCode);
                if (codes.length == 1) {
                  throw const ServerApiException(
                    '请输入两步验证码',
                    statusCode: 401,
                    code: 'totp_required',
                  );
                }
                return const AccountLoginResult(
                  token: 'tok',
                  username: 'alice',
                  expiresInMs: 3600000,
                );
              },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 0,
                  peers: <AddressBookPeer>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-totp')), findsNothing);
    await tester.enterText(find.byKey(const Key('account-username')), 'alice');
    await tester.enterText(find.byKey(const Key('account-password')), 'secret');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(find.text('请输入两步验证码'), findsOneWidget);
    expect(find.byKey(const Key('account-totp')), findsOneWidget);
    expect(accounts.session, isNull);

    await tester.enterText(find.byKey(const Key('account-totp')), '123456');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(codes, <String>['', '123456']);
    expect(accounts.session?.username, 'alice');
    expect(accounts.session?.token, 'tok');

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('account login can use a security key as the second factor', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final List<String> keys = <String>[];
    final _MemoryAccount accounts = _MemoryAccount();
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          accountLogin:
              (
                ServerSettings server, {
                required String username,
                required String password,
                String totpCode = '',
                String recoveryCode = '',
                String smsCode = '',
                String emailCode = '',
              }) async {
                throw const ServerApiException(
                  '请输入两步验证码',
                  statusCode: 401,
                  code: 'totp_required',
                );
              },
          accountSecurityKeyLogin:
              (
                ServerSettings server, {
                required String username,
                required String password,
              }) async {
                keys.add('$username|$password');
                return const AccountLoginResult(
                  token: 'keytok',
                  username: 'alice',
                  expiresInMs: 3600000,
                );
              },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 0,
                  peers: <AddressBookPeer>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-use-security-key')), findsNothing);
    await tester.enterText(find.byKey(const Key('account-username')), 'alice');
    await tester.enterText(find.byKey(const Key('account-password')), 'secret');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('account-use-security-key')), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-use-security-key')));
    await tester.pumpAndSettle();

    expect(keys, <String>['alice|secret']);
    expect(accounts.session?.username, 'alice');
    expect(accounts.session?.token, 'keytok');

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('account login can fall back to a recovery code', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final List<String> calls = <String>[];
    final _MemoryAccount accounts = _MemoryAccount();
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          accountLogin:
              (
                ServerSettings server, {
                required String username,
                required String password,
                String totpCode = '',
                String recoveryCode = '',
                String smsCode = '',
                String emailCode = '',
              }) async {
                calls.add('$totpCode|$recoveryCode|$smsCode');
                if (calls.length == 1) {
                  throw const ServerApiException(
                    '请输入两步验证码',
                    statusCode: 401,
                    code: 'totp_required',
                  );
                }
                return const AccountLoginResult(
                  token: 'tok',
                  username: 'alice',
                  expiresInMs: 3600000,
                );
              },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 0,
                  peers: <AddressBookPeer>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('account-username')), 'alice');
    await tester.enterText(find.byKey(const Key('account-password')), 'secret');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('account-totp')), findsOneWidget);
    await tester.tap(find.byKey(const Key('account-use-recovery')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-totp')), findsNothing);
    expect(find.byKey(const Key('account-recovery-code')), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();
    expect(find.text('请输入恢复码'), findsOneWidget);
    expect(calls, <String>['||']);

    await tester.enterText(
      find.byKey(const Key('account-recovery-code')),
      ' ABCD-EFGH-IJKL-MNOP ',
    );
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(calls, <String>['||', '|ABCD-EFGH-IJKL-MNOP|']);
    expect(accounts.session?.username, 'alice');
    expect(accounts.session?.token, 'tok');

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('account login can request and use an SMS backup code', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final List<String> calls = <String>[];
    final List<String> smsRequests = <String>[];
    final _MemoryAccount accounts = _MemoryAccount();
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          accountLogin:
              (
                ServerSettings server, {
                required String username,
                required String password,
                String totpCode = '',
                String recoveryCode = '',
                String smsCode = '',
                String emailCode = '',
              }) async {
                calls.add('$totpCode|$recoveryCode|$smsCode');
                if (calls.length == 1) {
                  throw const ServerApiException(
                    '请输入两步验证码',
                    statusCode: 401,
                    code: 'totp_required',
                  );
                }
                return const AccountLoginResult(
                  token: 'tok',
                  username: 'alice',
                  expiresInMs: 3600000,
                );
              },
          accountLoginSMS:
              (
                ServerSettings server, {
                required String username,
                required String password,
              }) async {
                smsRequests.add('$username|$password');
              },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 0,
                  peers: <AddressBookPeer>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('account-username')), 'alice');
    await tester.enterText(find.byKey(const Key('account-password')), 'secret');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-totp')), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-use-sms')));
    await tester.pumpAndSettle();
    expect(smsRequests, <String>['alice|secret']);
    expect(find.byKey(const Key('account-sms')), findsOneWidget);
    expect(find.byKey(const Key('account-totp')), findsNothing);
    expect(find.text('短信验证码已发送到绑定的手机号'), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();
    expect(find.text('请输入短信验证码'), findsOneWidget);
    expect(calls, <String>['||']);

    await tester.enterText(find.byKey(const Key('account-sms')), '1234 5678');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(calls, <String>['||', '||12345678']);
    expect(accounts.session?.username, 'alice');
    expect(accounts.session?.token, 'tok');

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('account login can request and use an email backup code', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final List<String> calls = <String>[];
    final List<String> emailRequests = <String>[];
    final _MemoryAccount accounts = _MemoryAccount();
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          accountLogin:
              (
                ServerSettings server, {
                required String username,
                required String password,
                String totpCode = '',
                String recoveryCode = '',
                String smsCode = '',
                String emailCode = '',
              }) async {
                calls.add('$totpCode|$recoveryCode|$smsCode|$emailCode');
                if (calls.length == 1) {
                  throw const ServerApiException(
                    '请输入两步验证码',
                    statusCode: 401,
                    code: 'totp_required',
                  );
                }
                return const AccountLoginResult(
                  token: 'tok',
                  username: 'alice',
                  expiresInMs: 3600000,
                );
              },
          accountLoginEmail:
              (
                ServerSettings server, {
                required String username,
                required String password,
              }) async {
                emailRequests.add('$username|$password');
              },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 0,
                  peers: <AddressBookPeer>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('account-username')), 'alice');
    await tester.enterText(find.byKey(const Key('account-password')), 'secret');
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-totp')), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-use-email')));
    await tester.pumpAndSettle();
    expect(emailRequests, <String>['alice|secret']);
    expect(find.byKey(const Key('account-email-code')), findsOneWidget);
    expect(find.byKey(const Key('account-totp')), findsNothing);
    expect(find.text('邮件验证码已发送到绑定的邮箱'), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();
    expect(find.text('请输入邮件验证码'), findsOneWidget);
    expect(calls, <String>['|||']);

    await tester.enterText(
      find.byKey(const Key('account-email-code')),
      '1234 5678',
    );
    await tester.tap(find.byKey(const Key('account-login')));
    await tester.pumpAndSettle();

    expect(calls, <String>['|||', '|||12345678']);
    expect(accounts.session?.username, 'alice');
    expect(accounts.session?.token, 'tok');

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('renaming and deleting a saved device updates the account', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
        bookRevision: 2,
      ),
    );
    final List<String> pushes = <String>[];
    final List<String> deletes = <String>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'lab',
              name: '旧名称',
              server: 'http://127.0.0.1:8443',
              saved: true,
              addedAt: DateTime.now(),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                // The first sync after launch reads the whole book so group names
                // are available even though the cached peer cursor is ahead.
                expect(since, 0);
                return const AddressBookSnapshot(
                  revision: 3,
                  peers: <AddressBookPeer>[],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add('$peerId|${name ?? ''}');
                return AddressBookPeer(id: peerId);
              },
          addressBookPeerDeleter:
              (ServerSettings server, String token, String peerId) async {
                deletes.add(peerId);
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }
    expect(pushes, <String>['lab|旧名称']);

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('重命名'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(TextField),
      ),
      '新名称',
    );
    await tester.tap(find.text('保存'));
    await tester.pumpAndSettle();
    expect(pushes, <String>['lab|旧名称', 'lab|新名称']);

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('删除'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('删除').last);
    await tester.pumpAndSettle();

    expect(deletes, <String>['lab']);
    expect(accounts.session?.token, 'tok');
    expect(accounts.session?.bookRevision, 3);
    expect(find.text('新名称'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('an expired access token is renewed from the refresh token', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final DateTime now = DateTime.now().toUtc();
    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'old-access',
        expiresAt: now.subtract(const Duration(minutes: 1)),
        refreshToken: 'refresh-value',
        refreshExpiresAt: now.add(const Duration(days: 30)),
      ),
    );
    final List<String> refreshed = <String>[];
    final List<String> fetchedTokens = <String>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async =>
              const BatchLookupResult(hosts: <String, HostInfo>{}),
          accountRefresh: (ServerSettings server, String refreshToken) async {
            refreshed.add(refreshToken);
            return const AccountLoginResult(
              token: 'new-access',
              username: 'alice',
              expiresInMs: 1800000,
              refreshToken: 'new-refresh',
              refreshExpiresInMs: 2592000000,
            );
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                fetchedTokens.add(token);
                return const AddressBookSnapshot(
                  revision: 1,
                  peers: <AddressBookPeer>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 8; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(refreshed, <String>['refresh-value']);
    expect(fetchedTokens, <String>['new-access']);
    expect(accounts.session?.token, 'new-access');
    expect(accounts.session?.refreshToken, 'new-refresh');
    expect(
      accounts.session!.refreshExpiresAt!.isAfter(
        now.add(const Duration(days: 29)),
      ),
      isTrue,
    );

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a rejected refresh token signs the viewer out', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final DateTime now = DateTime.now().toUtc();
    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'old-access',
        expiresAt: now.subtract(const Duration(minutes: 1)),
        refreshToken: 'retired-refresh',
        refreshExpiresAt: now.add(const Duration(days: 30)),
      ),
    );
    bool fetched = false;
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async =>
              const BatchLookupResult(hosts: <String, HostInfo>{}),
          accountRefresh: (ServerSettings server, String refreshToken) async {
            throw const ServerApiException('登录已过期，请重新登录', statusCode: 401);
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                fetched = true;
                return const AddressBookSnapshot(
                  revision: 1,
                  peers: <AddressBookPeer>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 8; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(accounts.session, isNull);
    expect(fetched, isFalse);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('wake sends a magic packet and remembers the MAC', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final DateTime now = DateTime.now();
    final _MemoryDevices devices = _MemoryDevices(<Device>[
      Device(
        id: 'devbox',
        name: '办公室',
        server: 'http://127.0.0.1:8443',
        saved: true,
        addedAt: now.subtract(const Duration(hours: 1)),
      ),
    ]);
    final List<String> sent = <String>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: devices,
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async =>
              const BatchLookupResult(hosts: <String, HostInfo>{}),
          wakeSender: (String mac) async => sent.add(mac),
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('唤醒'));
    await tester.pumpAndSettle();
    expect(find.text('局域网唤醒'), findsOneWidget);

    await tester.enterText(find.byKey(const Key('wake-mac')), 'nope');
    await tester.tap(find.text('唤醒').last);
    await tester.pumpAndSettle();
    expect(sent, isEmpty);
    expect(find.textContaining('12 位十六进制'), findsOneWidget);

    await tester.enterText(
      find.byKey(const Key('wake-mac')),
      'aa-bb-cc-dd-ee-ff',
    );
    await tester.tap(find.text('唤醒').last);
    await tester.pumpAndSettle();
    expect(sent, <String>['AA:BB:CC:DD:EE:FF']);
    expect(devices.devices.single.mac, 'AA:BB:CC:DD:EE:FF');
    expect(find.text('已发送局域网唤醒包'), findsOneWidget);
    expect(find.text('局域网唤醒'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('account tags can be edited locally and are uploaded', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
        bookRevision: 1,
      ),
    );
    final _MemoryDevices devices = _MemoryDevices(<Device>[
      Device(
        id: 'lab',
        name: '实验室',
        server: 'http://127.0.0.1:8443',
        tags: const <String>['vip'],
        saved: true,
        addedAt: DateTime.now(),
      ),
    ]);
    final List<String> pushes = <String>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: devices,
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                expect(since, 0);
                return const AddressBookSnapshot(
                  revision: 2,
                  peers: <AddressBookPeer>[
                    AddressBookPeer(
                      id: 'lab',
                      name: '实验室',
                      tags: <String>['vip'],
                    ),
                  ],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add('$peerId|${name ?? '-'}|${tags?.join('+') ?? '-'}');
                return AddressBookPeer(id: peerId);
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }
    expect(pushes, isEmpty);
    expect(find.textContaining('#vip'), findsOneWidget);

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('账号标签'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('account-tags')),
      'office, 共享, office',
    );
    await tester.tap(find.byKey(const Key('account-tags-save')));
    await tester.pumpAndSettle();

    expect(devices.devices.single.tags, <String>['office', '共享']);
    expect(pushes, <String>['lab|实验室|office+共享']);
    expect(find.textContaining('#office #共享'), findsOneWidget);
    expect(accounts.session?.bookRevision, 2);

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('账号标签'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('account-tags')), '');
    await tester.tap(find.byKey(const Key('account-tags-save')));
    await tester.pumpAndSettle();

    expect(devices.devices.single.tags, isEmpty);
    expect(pushes, <String>['lab|实验室|office+共享', 'lab|实验室|']);
    expect(find.textContaining('#'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('an offline tag clear is not pulled back by the next sync', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
        bookRevision: 4,
      ),
    );
    final _MemoryDevices devices = _MemoryDevices(<Device>[
      Device(
        id: 'lab',
        name: '实验室',
        server: 'http://127.0.0.1:8443',
        tags: const <String>['vip'],
        saved: true,
        addedAt: DateTime.now(),
      ),
    ]);
    bool offline = true;
    final List<List<String>?> pushes = <List<String>?>[];
    List<AddressBookPeer> serverPeers = <AddressBookPeer>[
      const AddressBookPeer(
        id: 'lab',
        name: '实验室',
        tags: <String>['vip'],
        revision: 4,
      ),
    ];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: devices,
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return AddressBookSnapshot(
                  revision: 4,
                  peers: List<AddressBookPeer>.of(serverPeers),
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add(tags);
                if (offline) {
                  throw const ServerApiException('网络不可用');
                }
                if (tags != null) {
                  serverPeers = <AddressBookPeer>[
                    AddressBookPeer(id: peerId, name: '实验室', tags: tags),
                  ];
                }
                return AddressBookPeer(
                  id: peerId,
                  name: '实验室',
                  tags: tags ?? const <String>[],
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }
    expect(pushes, isEmpty);
    expect(devices.devices.single.tags, <String>['vip']);

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('账号标签'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('account-tags')), '');
    await tester.tap(find.byKey(const Key('account-tags-save')));
    await tester.pumpAndSettle();

    expect(pushes, <List<String>?>[<String>[]]);
    expect(devices.devices.single.tags, isEmpty);
    expect(devices.devices.single.tagsCleared, isTrue);

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('account-sync')));
    await tester.pumpAndSettle();

    expect(pushes, hasLength(2));
    expect(pushes.last, isEmpty);
    expect(devices.devices.single.tags, isEmpty);
    expect(devices.devices.single.tagsCleared, isTrue);

    offline = false;
    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('account-sync')));
    await tester.pumpAndSettle();

    expect(pushes, hasLength(3));
    expect(pushes.last, isEmpty);
    expect(devices.devices.single.tags, isEmpty);
    expect(devices.devices.single.tagsCleared, isFalse);
    expect(find.textContaining('#'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('an offline group clear is not pulled back by the next sync', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
        bookRevision: 6,
      ),
    );
    final _MemoryDevices devices = _MemoryDevices(<Device>[
      Device(
        id: 'studio',
        name: '工作室',
        server: 'http://127.0.0.1:8443',
        groupId: 'work',
        saved: true,
        addedAt: DateTime.now(),
      ),
    ]);
    bool offline = true;
    final List<String?> pushes = <String?>[];
    String serverGroup = 'work';
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: devices,
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return AddressBookSnapshot(
                  revision: 6,
                  peers: <AddressBookPeer>[
                    AddressBookPeer(
                      id: 'studio',
                      name: '工作室',
                      groupId: serverGroup,
                      revision: 6,
                    ),
                  ],
                  groups: const <AddressBookGroup>[
                    AddressBookGroup(id: 'work', name: 'Work', revision: 2),
                  ],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add(groupId);
                if (offline) {
                  throw const ServerApiException('网络不可用');
                }
                if (groupId != null) {
                  serverGroup = groupId;
                }
                return AddressBookPeer(id: peerId, groupId: groupId ?? '');
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }
    expect(pushes, isEmpty);
    expect(find.byKey(const Key('device-group-work')), findsOneWidget);

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('移动到分组'));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('group-pick-none')));
    await tester.pumpAndSettle();

    expect(pushes, <String?>['']);
    expect(devices.devices.single.groupId, '');
    expect(devices.devices.single.groupCleared, isTrue);

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('account-sync')));
    await tester.pumpAndSettle();

    expect(pushes, hasLength(2));
    expect(pushes.last, '');
    expect(devices.devices.single.groupId, '');
    expect(devices.devices.single.groupCleared, isTrue);

    offline = false;
    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('account-sync')));
    await tester.pumpAndSettle();

    expect(pushes, hasLength(3));
    expect(pushes.last, '');
    expect(devices.devices.single.groupId, '');
    expect(devices.devices.single.groupCleared, isFalse);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a stale address-book edit adopts the newer server row', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
        bookRevision: 3,
      ),
    );
    final _MemoryDevices devices = _MemoryDevices(<Device>[
      Device(
        id: 'studio',
        name: '旧名',
        server: 'http://127.0.0.1:8443',
        tags: const <String>['local'],
        saved: true,
        addedAt: DateTime.now(),
      ),
    ]);
    int remoteRevision = 3;
    String remoteName = '旧名';
    List<String> remoteTags = const <String>['local'];
    bool conflict = false;
    final List<int?> revisions = <int?>[];
    final List<List<String>?> pushes = <List<String>?>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: devices,
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return AddressBookSnapshot(
                  revision: remoteRevision,
                  peers: <AddressBookPeer>[
                    AddressBookPeer(
                      id: 'studio',
                      name: remoteName,
                      tags: remoteTags,
                      revision: remoteRevision,
                    ),
                  ],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add(tags);
                revisions.add(ifRevision);
                if (conflict) {
                  throw const ServerApiException(
                    '地址簿条目已在其他设备修改',
                    statusCode: 409,
                  );
                }
                return AddressBookPeer(
                  id: peerId,
                  name: name ?? remoteName,
                  tags: tags ?? remoteTags,
                  groupId: groupId ?? '',
                  revision: remoteRevision + 1,
                );
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }
    expect(pushes, isEmpty);
    expect(devices.devices.single.revision, 3);

    // Another viewer writes the row while this page still holds revision 3.
    remoteRevision = 8;
    remoteName = '服务器名';
    remoteTags = const <String>['server'];
    conflict = true;

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('账号标签'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('account-tags')), 'newtag');
    await tester.tap(find.byKey(const Key('account-tags-save')));
    await tester.pumpAndSettle();
    // The local-edit snackbar is still on screen; let it expire so the
    // queued conflict notice becomes visible.
    await tester.pump(const Duration(seconds: 4));
    await tester.pumpAndSettle();

    expect(pushes, <List<String>?>[
      <String>['newtag'],
    ]);
    expect(revisions, <int?>[3]);
    expect(devices.devices.single.name, '服务器名');
    expect(devices.devices.single.tags, <String>['server']);
    expect(devices.devices.single.revision, 8);
    expect(find.textContaining('已刷新为服务器版本'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets(
    'a deletion tombstone removes the local peer without re-uploading',
    (WidgetTester tester) async {
      tester.view.physicalSize = const Size(1600, 1000);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);

      final _MemoryAccount accounts = _MemoryAccount(
        AccountSession(
          server: 'http://127.0.0.1:8443',
          username: 'alice',
          token: 'tok',
          expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
          bookRevision: 5,
        ),
      );
      final _MemoryDevices devices = _MemoryDevices(<Device>[
        Device(
          id: 'devbox',
          name: '办公室',
          server: 'http://127.0.0.1:8443',
          saved: true,
          addedAt: DateTime.now(),
        ),
      ]);
      final List<String> pushes = <String>[];
      await tester.pumpWidget(
        MaterialApp(
          home: HomePage(
            deviceStore: devices,
            serverStore: _MemoryServers(
              const ServerSettings(url: 'http://127.0.0.1:8443'),
            ),
            identityStore: _MemoryIdentity(),
            trustStore: _MemoryTrust(<String, String>{}),
            accountStore: accounts,
            lookupBatch: (ServerSettings server, List<String> hostIds) async {
              return const BatchLookupResult(hosts: <String, HostInfo>{});
            },
            addressBookFetcher:
                (ServerSettings server, String token, int since) async {
                  expect(since, 0);
                  return const AddressBookSnapshot(
                    revision: 8,
                    peers: <AddressBookPeer>[],
                    changes: <AddressBookPeer>[
                      AddressBookPeer(id: 'devbox', deleted: true, revision: 8),
                    ],
                  );
                },
            addressBookPeerWriter:
                (
                  ServerSettings server,
                  String token,
                  String peerId, {
                  String? name,
                  List<String>? tags,
                  String? groupId,
                  int? ifRevision,
                }) async {
                  pushes.add(peerId);
                  return AddressBookPeer(id: peerId);
                },
          ),
        ),
      );
      for (int i = 0; i < 4; ++i) {
        await tester.pump(const Duration(milliseconds: 20));
      }

      expect(pushes, isEmpty);
      expect(devices.devices, isEmpty);
      expect(accounts.session?.bookRevision, 8);
      expect(find.text('办公室'), findsNothing);

      await tester.pumpWidget(const SizedBox());
    },
  );

  testWidgets('an offline deletion is retried and not pulled back', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
      ),
    );
    final _MemoryDevices devices = _MemoryDevices(<Device>[
      Device(
        id: 'lab',
        name: '实验室',
        server: 'http://127.0.0.1:8443',
        saved: true,
        addedAt: DateTime.now(),
      ),
    ]);
    final List<String> deletes = <String>[];
    bool offline = true;
    int revision = 1;
    List<AddressBookPeer> serverPeers = <AddressBookPeer>[
      const AddressBookPeer(id: 'lab', name: '实验室', revision: 1),
    ];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: devices,
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return AddressBookSnapshot(
                  revision: revision,
                  peers: List<AddressBookPeer>.of(serverPeers),
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async => AddressBookPeer(id: peerId),
          addressBookPeerDeleter:
              (ServerSettings server, String token, String peerId) async {
                deletes.add(peerId);
                if (offline) {
                  throw const ServerApiException('网络不可用');
                }
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }
    expect(devices.devices, hasLength(1));
    expect(accounts.session?.bookRevision, 1);

    await tester.tap(find.byTooltip('设备操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('删除'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('删除').last);
    await tester.pumpAndSettle();

    expect(devices.devices, isEmpty);
    expect(devices.pending.single.id, 'lab');
    expect(devices.pending.single.username, 'alice');
    expect(deletes, <String>['lab']);

    revision = 2;
    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('account-sync')));
    await tester.pumpAndSettle();

    expect(deletes, <String>['lab', 'lab']);
    expect(devices.devices, isEmpty);
    expect(devices.pending, hasLength(1));
    expect(find.text('实验室'), findsNothing);
    expect(find.textContaining('待删除重试 1'), findsOneWidget);

    offline = false;
    serverPeers = <AddressBookPeer>[];
    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('account-sync')));
    await tester.pumpAndSettle();

    expect(deletes, <String>['lab', 'lab', 'lab']);
    expect(devices.pending, isEmpty);
    expect(devices.devices, isEmpty);
    expect(accounts.session?.bookRevision, 2);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a rejected account token returns to the login form', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'bob',
        token: 'stale',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
      ),
    );
    int fetches = 0;
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                fetches++;
                throw const ServerApiException('账号令牌无效', statusCode: 401);
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(fetches, greaterThan(0));
    expect(accounts.session, isNull);
    expect(find.text('登录账号'), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-username')), findsOneWidget);
    expect(find.byKey(const Key('account-status')), findsNothing);
    expect(find.byKey(const Key('account-forgot-password')), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-forgot-password')));
    await tester.pumpAndSettle();
    expect(find.text('找回密码'), findsOneWidget);
    expect(find.byKey(const Key('forgot-username')), findsOneWidget);
    await tester.tap(find.text('取消').last);
    await tester.pumpAndSettle();

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('signing out clears the stored session and token', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
      ),
    );
    String? revoked;
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 0,
                  peers: <AddressBookPeer>[],
                );
              },
          accountLogout: (ServerSettings server, String token) async {
            revoked = token;
          },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('account-tile')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-status')), findsOneWidget);
    expect(find.byKey(const Key('account-sessions')), findsOneWidget);
    expect(find.byKey(const Key('account-change-password')), findsOneWidget);
    expect(find.byKey(const Key('account-recovery-email')), findsOneWidget);
    expect(find.byKey(const Key('account-recovery-phone')), findsOneWidget);
    expect(find.byKey(const Key('account-two-factor')), findsOneWidget);
    await tester.tap(find.byKey(const Key('account-logout')));
    await tester.pumpAndSettle();

    expect(revoked, 'tok');
    expect(accounts.session, isNull);
    expect(find.text('登录账号'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets(
    'saved devices show server groups and can be moved between them',
    (WidgetTester tester) async {
      tester.view.physicalSize = const Size(1600, 1000);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);

      final DateTime now = DateTime.now();
      final _MemoryAccount accounts = _MemoryAccount(
        AccountSession(
          server: 'http://127.0.0.1:8443',
          username: 'alice',
          token: 'tok',
          expiresAt: now.toUtc().add(const Duration(hours: 1)),
          bookRevision: 4,
        ),
      );
      final _MemoryDevices devices = _MemoryDevices(<Device>[
        Device(
          id: 'studio',
          name: '工作室',
          server: 'http://127.0.0.1:8443',
          groupId: 'work',
          saved: true,
          addedAt: now.subtract(const Duration(hours: 1)),
        ),
        Device(
          id: 'lab',
          name: '实验室',
          server: 'http://127.0.0.1:8443',
          groupId: 'lab',
          saved: true,
          addedAt: now.subtract(const Duration(hours: 2)),
        ),
      ]);
      final List<String> pushes = <String>[];
      int fetches = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: HomePage(
            deviceStore: devices,
            serverStore: _MemoryServers(
              const ServerSettings(url: 'http://127.0.0.1:8443'),
            ),
            identityStore: _MemoryIdentity(),
            trustStore: _MemoryTrust(<String, String>{}),
            accountStore: accounts,
            lookupBatch: (ServerSettings server, List<String> hostIds) async {
              return const BatchLookupResult(hosts: <String, HostInfo>{});
            },
            addressBookFetcher:
                (ServerSettings server, String token, int since) async {
                  fetches++;
                  expect(since, 0);
                  return const AddressBookSnapshot(
                    revision: 5,
                    peers: <AddressBookPeer>[
                      AddressBookPeer(
                        id: 'studio',
                        name: '工作室',
                        groupId: 'work',
                      ),
                      AddressBookPeer(id: 'lab', name: '实验室', groupId: 'lab'),
                    ],
                    groups: <AddressBookGroup>[
                      AddressBookGroup(id: 'work', name: 'Work', revision: 2),
                      AddressBookGroup(id: 'lab', name: 'Lab', revision: 3),
                    ],
                  );
                },
            addressBookPeerWriter:
                (
                  ServerSettings server,
                  String token,
                  String peerId, {
                  String? name,
                  List<String>? tags,
                  String? groupId,
                  int? ifRevision,
                }) async {
                  pushes.add('$peerId|${name ?? ''}|${groupId ?? '-'}');
                  return AddressBookPeer(id: peerId);
                },
          ),
        ),
      );
      for (int i = 0; i < 6; ++i) {
        await tester.pump(const Duration(milliseconds: 20));
      }

      expect(fetches, 1);
      expect(pushes, isEmpty);
      expect(find.byKey(const Key('device-group-work')), findsOneWidget);
      expect(find.byKey(const Key('device-group-lab')), findsOneWidget);
      expect(find.byKey(const Key('device-group-ungrouped')), findsNothing);
      expect(find.text('Work'), findsOneWidget);
      expect(find.text('Lab'), findsOneWidget);

      final Finder studioRow = find
          .ancestor(of: find.text('工作室'), matching: find.byType(Row))
          .at(1);
      await tester.tap(
        find.descendant(of: studioRow, matching: find.byTooltip('设备操作')),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.text('移动到分组'));
      await tester.pumpAndSettle();

      expect(find.byKey(const Key('group-pick-none')), findsOneWidget);
      expect(find.byKey(const Key('group-pick-work')), findsOneWidget);
      expect(find.byKey(const Key('group-pick-lab')), findsOneWidget);
      await tester.tap(find.byKey(const Key('group-pick-lab')));
      await tester.pumpAndSettle();

      expect(pushes, <String>['studio|工作室|lab']);
      expect(
        devices.devices
            .firstWhere((Device item) => item.id == 'studio')
            .groupId,
        'lab',
      );
      expect(find.text('已移动到分组'), findsOneWidget);
      expect(find.byKey(const Key('device-group-work')), findsNothing);
      expect(find.byKey(const Key('device-group-lab')), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
    },
  );

  testWidgets('a saved peer adopts its server group on the first sync', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
        bookRevision: 9,
      ),
    );
    final _MemoryDevices devices = _MemoryDevices(<Device>[
      Device(
        id: 'studio',
        server: 'http://127.0.0.1:8443',
        saved: true,
        addedAt: DateTime.now(),
      ),
    ]);
    final List<String> pushes = <String>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: devices,
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                expect(since, 0);
                return const AddressBookSnapshot(
                  revision: 10,
                  peers: <AddressBookPeer>[
                    AddressBookPeer(id: 'studio', name: '工作室', groupId: 'work'),
                  ],
                  groups: <AddressBookGroup>[
                    AddressBookGroup(id: 'work', name: 'Work', revision: 9),
                  ],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add('$peerId|${name ?? ''}|${groupId ?? '-'}');
                return AddressBookPeer(id: peerId);
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(pushes, isEmpty);
    final Device studio = devices.devices.firstWhere(
      (Device item) => item.id == 'studio',
    );
    expect(studio.name, '工作室');
    expect(studio.groupId, 'work');
    expect(find.byKey(const Key('device-group-work')), findsOneWidget);
    expect(find.byKey(const Key('device-group-ungrouped')), findsNothing);
    expect(find.text('Work'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('the groups button opens management with member counts', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
        bookRevision: 2,
      ),
    );
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'studio',
              name: '工作室',
              server: 'http://127.0.0.1:8443',
              groupId: 'work',
              saved: true,
              addedAt: DateTime.now(),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 2,
                  peers: <AddressBookPeer>[
                    AddressBookPeer(id: 'studio', name: '工作室', groupId: 'work'),
                  ],
                  groups: <AddressBookGroup>[
                    AddressBookGroup(id: 'work', name: 'Work', revision: 2),
                  ],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async => AddressBookPeer(id: peerId),
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    await tester.tap(find.byKey(const Key('manage-groups')));
    await tester.pumpAndSettle();
    expect(find.text('地址簿分组'), findsOneWidget);
    expect(find.byKey(const Key('group-row-work')), findsOneWidget);
    expect(find.text('1 台设备'), findsOneWidget);
    await tester.tap(find.text('关闭'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('group-row-work')), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('the address book filters by name, id and tags', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final DateTime now = DateTime.now();
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'studio',
              name: '工作室',
              server: 'http://127.0.0.1:8443',
              tags: const <String>['office', 'vip'],
              saved: true,
              addedAt: now,
            ),
            Device(
              id: 'lab',
              name: '实验室',
              server: 'http://127.0.0.1:8443',
              tags: const <String>['lab'],
              saved: true,
              addedAt: now.subtract(const Duration(minutes: 1)),
            ),
            Device(
              id: '10.0.0.9:4433',
              saved: true,
              addedAt: now.subtract(const Duration(minutes: 2)),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(find.byKey(const Key('device-search')), findsOneWidget);
    expect(find.byKey(const Key('tag-filter-all')), findsOneWidget);
    expect(find.byKey(const Key('tag-filter-lab')), findsOneWidget);
    expect(find.byKey(const Key('tag-filter-office')), findsOneWidget);
    expect(find.byKey(const Key('tag-filter-vip')), findsOneWidget);
    expect(_savedCount(tester), '3');

    await tester.enterText(find.byKey(const Key('device-search')), '实验');
    await tester.pump();
    expect(_savedCount(tester), '1/3');
    expect(find.text('实验室'), findsOneWidget);
    expect(find.text('工作室'), findsNothing);
    expect(find.text('10.0.0.9:4433'), findsNothing);

    await tester.tap(find.byKey(const Key('device-search-clear')));
    await tester.pump();
    expect(_savedCount(tester), '3');
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('实验室'), findsOneWidget);

    await tester.tap(find.byKey(const Key('tag-filter-vip')));
    await tester.pump();
    expect(_savedCount(tester), '1/3');
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('实验室'), findsNothing);
    expect(find.text('10.0.0.9:4433'), findsNothing);

    await tester.tap(find.byKey(const Key('tag-filter-lab')));
    await tester.pump();
    expect(_savedCount(tester), '2/3');
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('实验室'), findsOneWidget);
    expect(find.text('10.0.0.9:4433'), findsNothing);

    await tester.tap(find.byKey(const Key('tag-filter-all')));
    await tester.pump();
    expect(_savedCount(tester), '3');

    await tester.enterText(find.byKey(const Key('device-search')), 'OFFICE');
    await tester.pump();
    expect(_savedCount(tester), '1/3');
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('实验室'), findsNothing);

    await tester.enterText(find.byKey(const Key('device-search')), '10.0.0.9');
    await tester.pump();
    expect(_savedCount(tester), '1/3');
    expect(find.text('10.0.0.9:4433'), findsWidgets);
    expect(find.text('工作室'), findsNothing);

    await tester.enterText(find.byKey(const Key('device-search')), '不存在');
    await tester.pump();
    expect(_savedCount(tester), '0/3');
    expect(find.text('没有匹配的设备'), findsOneWidget);
    expect(find.text('工作室'), findsNothing);

    await tester.tap(find.byKey(const Key('device-filter-reset')));
    await tester.pump();
    expect(_savedCount(tester), '3');
    expect(find.text('没有匹配的设备'), findsNothing);
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('实验室'), findsOneWidget);
    expect(find.byKey(const Key('device-search-clear')), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('filters narrow grouped devices and their headers', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final DateTime now = DateTime.now();
    final _MemoryAccount accounts = _MemoryAccount(
      AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'tok',
        expiresAt: now.toUtc().add(const Duration(hours: 1)),
        bookRevision: 4,
      ),
    );
    final List<String> pushes = <String>[];
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'studio',
              name: '工作室',
              server: 'http://127.0.0.1:8443',
              tags: const <String>['office'],
              groupId: 'work',
              saved: true,
              addedAt: now,
            ),
            Device(
              id: 'lab',
              name: '实验室',
              server: 'http://127.0.0.1:8443',
              tags: const <String>['lab'],
              groupId: 'lab',
              saved: true,
              addedAt: now.subtract(const Duration(minutes: 1)),
            ),
            Device(
              id: 'devbox',
              name: '机房',
              server: 'http://127.0.0.1:8443',
              tags: const <String>['office'],
              saved: true,
              addedAt: now.subtract(const Duration(minutes: 2)),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: accounts,
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(hosts: <String, HostInfo>{});
          },
          addressBookFetcher:
              (ServerSettings server, String token, int since) async {
                return const AddressBookSnapshot(
                  revision: 5,
                  peers: <AddressBookPeer>[
                    AddressBookPeer(
                      id: 'studio',
                      name: '工作室',
                      tags: <String>['office'],
                      groupId: 'work',
                    ),
                    AddressBookPeer(
                      id: 'lab',
                      name: '实验室',
                      tags: <String>['lab'],
                      groupId: 'lab',
                    ),
                    AddressBookPeer(
                      id: 'devbox',
                      name: '机房',
                      tags: <String>['office'],
                    ),
                  ],
                  groups: <AddressBookGroup>[
                    AddressBookGroup(id: 'work', name: 'Work', revision: 2),
                    AddressBookGroup(id: 'lab', name: 'Lab', revision: 3),
                  ],
                );
              },
          addressBookPeerWriter:
              (
                ServerSettings server,
                String token,
                String peerId, {
                String? name,
                List<String>? tags,
                String? groupId,
                int? ifRevision,
              }) async {
                pushes.add(peerId);
                return AddressBookPeer(id: peerId);
              },
        ),
      ),
    );
    for (int i = 0; i < 6; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(pushes, isEmpty);
    expect(find.byKey(const Key('device-group-work')), findsOneWidget);
    expect(find.byKey(const Key('device-group-lab')), findsOneWidget);
    expect(find.byKey(const Key('device-group-ungrouped')), findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(const Key('device-group-work')),
        matching: find.text('1'),
      ),
      findsOneWidget,
    );

    await tester.enterText(find.byKey(const Key('device-search')), '实验');
    await tester.pump();
    expect(_savedCount(tester), '1/3');
    expect(find.byKey(const Key('device-group-work')), findsNothing);
    expect(find.byKey(const Key('device-group-lab')), findsOneWidget);
    expect(find.byKey(const Key('device-group-ungrouped')), findsNothing);
    expect(find.text('实验室'), findsOneWidget);
    expect(find.text('工作室'), findsNothing);

    await tester.tap(find.byKey(const Key('device-search-clear')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('tag-filter-office')));
    await tester.pump();
    expect(_savedCount(tester), '2/3');
    expect(find.byKey(const Key('device-group-work')), findsOneWidget);
    expect(find.byKey(const Key('device-group-lab')), findsNothing);
    expect(find.byKey(const Key('device-group-ungrouped')), findsOneWidget);
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('机房'), findsOneWidget);
    expect(find.text('实验室'), findsNothing);

    await tester.tap(find.byKey(const Key('tag-filter-lab')));
    await tester.pump();
    expect(_savedCount(tester), '3/3');
    expect(find.byKey(const Key('device-group-lab')), findsOneWidget);

    await tester.enterText(find.byKey(const Key('device-search')), '工作');
    await tester.pump();
    expect(_savedCount(tester), '1/3');
    expect(find.byKey(const Key('device-group-work')), findsOneWidget);
    expect(find.byKey(const Key('device-group-lab')), findsNothing);
    expect(find.byKey(const Key('device-group-ungrouped')), findsNothing);
    expect(find.text('工作室'), findsOneWidget);

    await tester.tap(find.byKey(const Key('device-filter-reset')));
    await tester.pump();
    expect(_savedCount(tester), '3');
    expect(find.byKey(const Key('device-group-work')), findsOneWidget);
    expect(find.byKey(const Key('device-group-lab')), findsOneWidget);
    expect(find.byKey(const Key('device-group-ungrouped')), findsOneWidget);
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('实验室'), findsOneWidget);
    expect(find.text('机房'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('host aliases and host tags are searchable and filterable', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'remote',
              server: 'http://127.0.0.1:8443',
              saved: true,
              addedAt: DateTime.now(),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          identityStore: _MemoryIdentity(),
          trustStore: _MemoryTrust(<String, String>{}),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            return const BatchLookupResult(
              hosts: <String, HostInfo>{
                'remote': HostInfo(
                  id: 'remote',
                  online: true,
                  alias: '会议室',
                  tags: <String>['prod'],
                ),
              },
            );
          },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(find.text('会议室'), findsOneWidget);
    expect(find.byKey(const Key('tag-filter-prod')), findsOneWidget);

    await tester.enterText(find.byKey(const Key('device-search')), 'PROD');
    await tester.pump();
    expect(_savedCount(tester), '1/1');
    expect(find.text('会议室'), findsOneWidget);

    await tester.enterText(find.byKey(const Key('device-search')), '会议');
    await tester.pump();
    expect(_savedCount(tester), '1/1');
    expect(find.text('会议室'), findsOneWidget);

    await tester.enterText(find.byKey(const Key('device-search')), '无匹配');
    await tester.pump();
    await tester.tap(find.byKey(const Key('device-filter-reset')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('tag-filter-prod')));
    await tester.pump();
    expect(_savedCount(tester), '1/1');
    expect(find.text('会议室'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a changed server key blocks presence lookups', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final ServerIdentity pinned = ServerIdentity.parse(
      'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8',
    )!;
    int lookups = 0;
    await tester.pumpWidget(
      MaterialApp(
        home: HomePage(
          deviceStore: _MemoryDevices(<Device>[
            Device(
              id: 'devbox',
              name: '办公室',
              server: 'http://127.0.0.1:8443',
              saved: true,
              addedAt: DateTime.now(),
            ),
          ]),
          serverStore: _MemoryServers(
            const ServerSettings(url: 'http://127.0.0.1:8443'),
          ),
          trustStore: _MemoryTrust(<String, String>{}),
          identityStore: _MemoryIdentity(<String, ServerIdentityPin>{
            'http://127.0.0.1:8443': ServerIdentityPin(
              identity: pinned,
              pinnedAt: DateTime.now(),
            ),
          }),
          accountStore: _MemoryAccount(),
          lookupBatch: (ServerSettings server, List<String> hostIds) async {
            lookups++;
            return BatchLookupResult(
              hosts: <String, HostInfo>{
                for (final String hostId in hostIds)
                  hostId: HostInfo(
                    id: hostId,
                    online: true,
                    serverPublicKey:
                        'Hx4dHBsaGRgXFhUUExIREA8ODQwLCgkIBwYFBAMCAQA',
                  ),
              },
            );
          },
        ),
      ),
    );
    for (int i = 0; i < 4; ++i) {
      await tester.pump(const Duration(milliseconds: 20));
    }

    expect(lookups, greaterThan(0));
    expect(find.text('服务不可达'), findsWidgets);
    expect(find.textContaining('在线'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });
}
