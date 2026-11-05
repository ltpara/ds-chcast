import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/rdp.dart';

void main() {
  test('the RDP client targets the viewer listener', () {
    expect(rdpClientArguments(3390), <String>['/v:127.0.0.1:3390']);
  });

  test('the RDP target keeps the random listener port as-is', () {
    expect(rdpClientArguments(61542), <String>['/v:127.0.0.1:61542']);
  });
}
