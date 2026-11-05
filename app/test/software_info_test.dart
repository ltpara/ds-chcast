import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_api.dart';

void main() {
  test('software notice carries the publisher signature', () {
    final SoftwareInfo? info = SoftwareInfo.fromJson(<String, Object?>{
      'version': '2.2.0',
      'url': 'https://downloads.example.com/software/chast-2.2.0.exe',
      'sha256': 'a' * 64,
      'size': 1024,
      'signature': 'server-signature',
      'publisher_key_id': '0123456789abcdef',
      'publisher_sig': 'publisher-signature',
    });
    expect(info, isNotNull);
    expect(info!.publisherKeyId, '0123456789abcdef');
    expect(info.publisherSignature, 'publisher-signature');
    expect(info.installable, isTrue);
  });

  test('a legacy notice without a publisher signature still parses', () {
    final SoftwareInfo? info = SoftwareInfo.fromJson(<String, Object?>{
      'version': '2.1.0',
      'url': 'https://downloads.example.com/software/chast-2.1.0.exe',
      'sha256': 'a' * 64,
      'size': 1024,
      'signature': 'server-signature',
    });
    expect(info, isNotNull);
    expect(info!.publisherKeyId, isEmpty);
    expect(info.publisherSignature, isEmpty);
    expect(info.installable, isTrue);
  });
}
