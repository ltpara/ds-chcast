import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the Ctrl+Alt+Del answer', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'sasCode': 2,
      'sasError': 'Ctrl+Alt+Del is only available through the Windows service',
      'sasUpdates': 4,
    });
    expect(status.sasCode, ViewerSasCode.unsupported.code);
    expect(status.sasError, contains('service'));
    expect(status.sasUpdates, 4);
  });

  test('a success has the Ok code and no reason', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'sasCode': 0,
      'sasError': '',
      'sasUpdates': 1,
    });
    expect(status.sasCode, ViewerSasCode.ok.code);
    expect(status.sasError, isEmpty);
    expect(status.sasUpdates, 1);
  });

  test('SAS fields default to zero when absent', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.sasCode, ViewerSasCode.ok.code);
    expect(status.sasError, isEmpty);
    expect(status.sasUpdates, 0);
  });

  test('every host code maps to its viewer enum', () {
    expect(ViewerSasCode.ok.code, 0);
    expect(ViewerSasCode.denied.code, 1);
    expect(ViewerSasCode.unsupported.code, 2);
    expect(ViewerSasCode.failed.code, 3);
  });
}
