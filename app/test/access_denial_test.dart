import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('each refusal code gets its own explanation', () {
    expect(accessDenialText(ViewerDenial.invalidToken), contains('令牌不正确'));
    expect(accessDenialText(ViewerDenial.tokenUsed), contains('已被使用'));
    expect(accessDenialText(ViewerDenial.declined), contains('拒绝'));
    expect(accessDenialText(ViewerDenial.wrongPassword), contains('密码不正确'));
    expect(accessDenialText(ViewerDenial.passwordRequired), contains('密码'));
    expect(accessDenialText(ViewerDenial.twoFactorRequired), contains('两步验证'));
    expect(accessDenialText(ViewerDenial.wrongTwoFactor), contains('验证码不正确'));
    expect(accessDenialText(ViewerDenial.lockedOut), contains('尝试次数过多'));
    expect(accessDenialText(ViewerDenial.notWhitelisted), contains('允许列表'));
    expect(accessDenialText(ViewerDenial.authTimeout), contains('限定时间'));
  });

  test('an absent or unknown code has no explanation', () {
    expect(accessDenialText(0), isEmpty);
    expect(accessDenialText(99), isEmpty);
  });

  test('status carries the denial code', () {
    expect(ViewerStatus.fromMap(<Object?, Object?>{'denial': 2}).denial, 2);
    expect(ViewerStatus.fromMap(<Object?, Object?>{}).denial, 0);
  });

  test('status carries the permission mask and its update count', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'permissions': ViewerPermission.input | ViewerPermission.audio,
      'permissionUpdates': 3,
      'inputEnabled': true,
    });
    expect(status.permissions, ViewerPermission.input | ViewerPermission.audio);
    expect(status.permissionUpdates, 3);
    expect(status.acceptsInput, isFalse, reason: 'no stream is live in this map');
    expect(ViewerStatus.fromMap(<Object?, Object?>{}).permissions, 0);
    expect(ViewerStatus.fromMap(<Object?, Object?>{}).permissionUpdates, 0);
  });

  test('a token travels with the target but stays out of its label', () {
    final ConnectTarget? target =
        parseConnectTarget('10.0.0.2:4433', token: '  one-shot  ');
    expect(target, isNotNull);
    expect(target!.token, 'one-shot');
    expect(target.label, '10.0.0.2:4433');
  });
}
