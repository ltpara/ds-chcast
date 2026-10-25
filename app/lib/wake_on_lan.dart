import 'dart:io';
import 'dart:typed_data';

/// Standard Wake-on-LAN destination port.
const int wakeOnLanPort = 9;

/// Sends one UDP wake packet. Injected by [HomePage] so widget tests never
/// touch the network.
typedef WakeSender = Future<void> Function(String mac);

/// Returns `AA:BB:CC:DD:EE:FF`, or null when [value] is not 12 hex digits with
/// optional `:`, `-`, `.` or whitespace separators.
String? normalizeMac(String value) {
  final String hex = value.replaceAll(RegExp(r'[:\-.\s]'), '').toUpperCase();
  if (!RegExp(r'^[0-9A-F]{12}$').hasMatch(hex)) {
    return null;
  }
  return <String>[
    for (int i = 0; i < 12; i += 2) hex.substring(i, i + 2),
  ].join(':');
}

/// Builds the 102-byte magic packet: six 0xFF bytes then the MAC 16 times.
Uint8List magicPacket(String mac) {
  final String? normalized = normalizeMac(mac);
  if (normalized == null) {
    throw const FormatException('invalid MAC address');
  }
  final List<int> bytes = <int>[
    for (final String part in normalized.split(':')) int.parse(part, radix: 16),
  ];
  final Uint8List packet = Uint8List(6 + 16 * 6);
  packet.fillRange(0, 6, 0xFF);
  for (int repeat = 0; repeat < 16; repeat++) {
    for (int i = 0; i < 6; i++) {
      packet[6 + repeat * 6 + i] = bytes[i];
    }
  }
  return packet;
}

class WakeOnLan {
  /// Broadcasts a magic packet on the viewer's local network. The target must
  /// be in that network and have Wake-on-LAN enabled in its firmware.
  static Future<void> send(
    String mac, {
    InternetAddress? address,
    int port = wakeOnLanPort,
  }) async {
    final Uint8List packet = magicPacket(mac);
    final RawDatagramSocket socket =
        await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
    try {
      socket.broadcastEnabled = true;
      final int sent = socket.send(
        packet,
        address ?? InternetAddress('255.255.255.255'),
        port,
      );
      if (sent != packet.length) {
        throw SocketException('wake packet was not sent');
      }
    } finally {
      socket.close();
    }
  }
}
