import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/wake_on_lan.dart';

void main() {
  test('normalizes common MAC formats', () {
    expect(normalizeMac('AA:BB:CC:DD:EE:FF'), 'AA:BB:CC:DD:EE:FF');
    expect(normalizeMac('aa-bb-cc-dd-ee-ff'), 'AA:BB:CC:DD:EE:FF');
    expect(normalizeMac('aabb.ccdd.eeff'), 'AA:BB:CC:DD:EE:FF');
    expect(normalizeMac(' aa bb cc dd ee ff '), 'AA:BB:CC:DD:EE:FF');
    expect(normalizeMac('aa:bb:cc:dd:ee'), isNull);
    expect(normalizeMac('gg:bb:cc:dd:ee:ff'), isNull);
    expect(normalizeMac(''), isNull);
  });

  test('magic packet is six FF bytes plus sixteen MAC copies', () {
    final Uint8List packet = magicPacket('aa-bb-cc-dd-ee-ff');
    expect(packet, hasLength(102));
    expect(packet.sublist(0, 6), everyElement(0xFF));
    const List<int> mac = <int>[0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF];
    for (int repeat = 0; repeat < 16; repeat++) {
      expect(packet.sublist(6 + repeat * 6, 12 + repeat * 6), mac);
    }
    expect(() => magicPacket('not-a-mac'), throwsFormatException);
  });

  test('sends a magic packet over UDP', () async {
    final RawDatagramSocket receiver =
        await RawDatagramSocket.bind(InternetAddress.loopbackIPv4, 0);
    addTearDown(receiver.close);
    final Completer<Uint8List> received = Completer<Uint8List>();
    receiver.listen((RawSocketEvent event) {
      if (event != RawSocketEvent.read || received.isCompleted) {
        return;
      }
      final Datagram? datagram = receiver.receive();
      if (datagram != null) {
        received.complete(datagram.data);
      }
    });

    await WakeOnLan.send(
      'AA:BB:CC:DD:EE:FF',
      address: InternetAddress.loopbackIPv4,
      port: receiver.port,
    );
    final Uint8List data =
        await received.future.timeout(const Duration(seconds: 3));
    expect(data, magicPacket('AA:BB:CC:DD:EE:FF'));
  });
}
