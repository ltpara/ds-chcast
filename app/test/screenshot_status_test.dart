import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the screenshot counter, path and error', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'screenshotUpdates': 4,
      'screenshotPath': r'C:\Users\me\Pictures\chast\chast-20261101-125500.png',
      'screenshotError': '',
    });
    expect(status.screenshotUpdates, 4);
    expect(status.screenshotPath, endsWith('chast-20261101-125500.png'));
    expect(status.screenshotError, isEmpty);
  });

  test('screenshots start with no count, no file and no error', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'connecting',
    });
    expect(status.screenshotUpdates, 0);
    expect(status.screenshotPath, '');
    expect(status.screenshotError, '');
  });

  test('a failed screenshot keeps its reason', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'screenshotUpdates': 1,
      'screenshotError': 'no remote picture has been rendered yet',
    });
    expect(status.screenshotUpdates, 1);
    expect(status.screenshotPath, isEmpty);
    expect(status.screenshotError, contains('no remote picture'));
  });
}
