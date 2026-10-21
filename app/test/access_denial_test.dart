import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('each refusal code gets its own explanation', () {
    expect(accessDenialText(ViewerDenial.invalidToken), contains('令牌不正确'));
    expect(accessDenialText(ViewerDenial.tokenUsed), contains('已被使用'));
    expect(accessDenialText(ViewerDenial.declined), contains('拒绝'));
  });

  test('an absent or unknown code has no explanation', () {
    expect(accessDenialText(0), isEmpty);
    expect(accessDenialText(99), isEmpty);
  });

  test('status carries the denial code', () {
    expect(ViewerStatus.fromMap(<Object?, Object?>{'denial': 2}).denial, 2);
    expect(ViewerStatus.fromMap(<Object?, Object?>{}).denial, 0);
  });

  test('a token travels with the target but stays out of its label', () {
    final ConnectTarget? target =
        parseConnectTarget('10.0.0.2:4433', token: '  one-shot  ');
    expect(target, isNotNull);
    expect(target!.token, 'one-shot');
    expect(target.label, '10.0.0.2:4433');
  });
}
