import 'dart:io';
import 'dart:isolate';

import 'package:flutter_test/flutter_test.dart';
import 'package:chast_viewer/device_store.dart';
import 'package:chast_viewer/store_lock.dart';
import 'package:chast_viewer/trust_store.dart';

Future<void> writeWindowStores(String root, int window) async {
  final TrustStore trust = TrustStore(file: File('$root/trust.json'));
  final DeviceStore devices = DeviceStore(file: File('$root/devices.json'));
  for (int i = 0; i < 12; ++i) {
    final String id = 'host-$window-$i:4433';
    await trust.trust(id, 'AB' * 32);
    await devices.save(Device(id: id, addedAt: DateTime(2026)));
    await devices.touch(Device(id: id, addedAt: DateTime(2026)));
  }
}

void main() {
  test('windows preserve each others pins and recent devices', () async {
    final Directory dir =
        await Directory.systemTemp.createTemp('chast-windows-');
    addTearDown(() => dir.delete(recursive: true));
    final String root = dir.path;
    await Future.wait(<Future<void>>[
      for (int i = 0; i < 3; ++i) Isolate.run(() => writeWindowStores(root, i)),
    ]);
    final Map<String, String> pins =
        await TrustStore(file: File('$root/trust.json')).load();
    final List<Device> devices =
        await DeviceStore(file: File('$root/devices.json')).load();
    expect(pins.length, 36);
    expect(devices.length, 36);
    expect(devices.map((Device d) => d.id).toSet(), pins.keys.toSet());
  });

  test('nested operations and a failed transaction release their locks',
      () async {
    final Directory dir = await Directory.systemTemp.createTemp('chast-lock-');
    addTearDown(() => dir.delete(recursive: true));
    final File file = File('${dir.path}/store.json');
    await expectLater(
      withStoreLock<void>(file, () async {
        await withStoreLock<void>(file, () async {
          throw StateError('write failed');
        });
      }),
      throwsStateError,
    );
    expect(await withStoreLock(file, () async => 7), 7);
    expect(withStoreLockSync(file, () => withStoreLockSync(file, () => 9)), 9);
  });
}
