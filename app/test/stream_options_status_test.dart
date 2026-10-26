import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the host answer and its effective values', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'codec': 'h264',
      'framerate': 15,
      'streamQuality': 1,
      'streamOptionCode': 0,
      'streamOptionError': '',
      'streamOptionResults': 3,
    });
    expect(status.streamQuality, ViewerStreamQuality.balanced.code);
    expect(status.streamOptionCode, ViewerStreamOptionsCode.ok.code);
    expect(status.streamOptionError, isEmpty);
    expect(status.streamOptionResults, 3);
    expect(status.framerate, 15);
  });

  test('a refusal keeps the reason and the effective quality', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'codec': 'h264',
      'framerate': 30,
      'streamQuality': 0,
      'streamOptionCode': ViewerStreamOptionsCode.unsupported.code,
      'streamOptionError': 'no HEVC encoder',
      'streamOptionResults': 1,
    });
    expect(status.streamOptionCode, ViewerStreamOptionsCode.unsupported.code);
    expect(status.streamOptionError, 'no HEVC encoder');
    expect(status.streamQuality, ViewerStreamQuality.high.code);
  });

  test('the stream-option fields default to the host defaults', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.streamQuality, ViewerStreamQuality.high.code);
    expect(status.streamOptionCode, ViewerStreamOptionsCode.ok.code);
    expect(status.streamOptionError, isEmpty);
    expect(status.streamOptionResults, 0);
  });

  test('menu values match the wire codes', () {
    expect(ViewerStreamCodec.auto.code, 0);
    expect(ViewerStreamCodec.h264.code, 1);
    expect(ViewerStreamCodec.hevc.code, 2);
    expect(ViewerStreamQuality.high.code, 0);
    expect(ViewerStreamQuality.balanced.code, 1);
    expect(ViewerStreamQuality.smooth.code, 2);
    expect(ViewerStreamOptionsCode.unsupported.code, 1);
    expect(ViewerStreamOptionsCode.failed.code, 2);
    expect(ViewerStreamOptionsCode.busy.code, 3);
  });
}
