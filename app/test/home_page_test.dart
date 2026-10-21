import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/device_store.dart';
import 'package:chast_viewer/pages/home_page.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/trust_store.dart';

class _MemoryDevices extends DeviceStore {
  _MemoryDevices(this.devices);

  final List<Device> devices;

  @override
  Future<List<Device>> load() async => List<Device>.of(devices);
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

void main() {
  testWidgets('home shows the address book, recent entries and service status',
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
      Device(
        id: '10.0.0.2:4433',
        addedAt: now,
        lastUsed: now,
      ),
    ];

    int lookups = 0;
    await tester.pumpWidget(MaterialApp(
      home: HomePage(
        deviceStore: _MemoryDevices(devices),
        serverStore: _MemoryServers(
          const ServerSettings(url: 'http://127.0.0.1:8443'),
        ),
        trustStore: _MemoryTrust(<String, String>{
          'server:http://127.0.0.1:8443/devbox': 'AB' * 32,
        }),
        lookup: (ServerSettings server, String hostId) async {
          lookups++;
          return HostInfo(
            id: hostId,
            online: true,
            candidate: '127.0.0.1:14434',
            relay: '127.0.0.1:41000',
          );
        },
      ),
    ));
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
  });
}
