import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/reconnect_policy.dart';

void main() {
  test('planned elevation covers rollback but still stops on auth failure', () {
    final policy = AutoReconnectPolicy()..reset(hostRestart: true);
    expect(policy.attemptLimit, 8);
    var seconds = 0;
    for (var i = 0; i < policy.attemptLimit; i++) {
      seconds += policy.next()!.inSeconds;
    }
    expect(seconds, greaterThan(60));
    expect(policy.next(), isNull);
    policy.reset(hostRestart: true);
    policy.next();
    expect(policy.afterStartupFailure(retryable: false), isNull);
    expect(policy.attemptLimit, AutoReconnectPolicy.limit);
    expect(policy.waiting, isFalse);
  });
  test('temporary startup failures continue the existing bounded series', () {
    final AutoReconnectPolicy policy = AutoReconnectPolicy();
    expect(policy.afterStartupFailure(retryable: true), isNull);
    expect(policy.attempt, 0);
    expect(policy.next(), const Duration(seconds: 1));
    for (final int seconds in <int>[2, 4, 8, 15]) {
      expect(
        policy.afterStartupFailure(retryable: true),
        Duration(seconds: seconds),
      );
    }
    expect(policy.afterStartupFailure(retryable: true), isNull);
    expect(policy.waiting, isFalse);
    expect(policy.attempt, 5);
    expect(policy.afterStartupFailure(retryable: true), isNull);
  });

  test('identity or authentication failure stops an active series', () {
    final AutoReconnectPolicy policy = AutoReconnectPolicy()..next();
    expect(policy.afterStartupFailure(retryable: false), isNull);
    expect(policy.waiting, isFalse);
    expect(policy.afterStartupFailure(retryable: true), isNull);
  });

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
