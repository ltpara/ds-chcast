import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/reconnect_policy.dart';

void main() {
  test('the ladder is 1/2/4/8/15 seconds and stops at five', () {
    final AutoReconnectPolicy policy = AutoReconnectPolicy();
    expect(policy.attempt, 0);
    expect(policy.waiting, isFalse);

    for (final int seconds in <int>[1, 2, 4, 8, 15]) {
      expect(policy.next(), Duration(seconds: seconds));
      expect(policy.waiting, isTrue);
    }
    expect(policy.attempt, AutoReconnectPolicy.limit);
    expect(policy.next(), isNull);
    expect(policy.waiting, isFalse);
  });

  test('a first frame resets the series', () {
    final AutoReconnectPolicy policy = AutoReconnectPolicy();
    policy.next();
    policy.next();
    policy.next();
    expect(policy.attempt, 3);

    policy.reset();
    expect(policy.attempt, 0);
    expect(policy.waiting, isFalse);
    expect(policy.next(), const Duration(seconds: 1));
  });

  test('cancelling the wait keeps the attempt count until a manual reset', () {
    final AutoReconnectPolicy policy = AutoReconnectPolicy();
    policy.next();

    // The page clears only the waiting flag when the timer is cancelled by a
    // manual reconnect; the count stays so the caller decides when to reset.
    policy.waiting = false;
    expect(policy.attempt, 1);
    expect(policy.next(), const Duration(seconds: 2));
  });
}
