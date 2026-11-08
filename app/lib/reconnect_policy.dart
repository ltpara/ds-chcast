/// The backoff ladder for an unexpected viewer drop.
///
/// The page decides when a drop qualifies (a frame was seen, no denial, no
/// identity change) and asks for one attempt at a time. Keeping the ladder
/// here makes the schedule and the five-attempt cap testable without a
/// native session.
class AutoReconnectPolicy {
  static const List<int> backoffSeconds = <int>[1, 2, 4, 8, 15];
  static const int limit = 5;
  static const List<int> _hostRestartBackoffSeconds = <int>[
    1,
    2,
    4,
    8,
    15,
    15,
    15,
    15,
  ];
  bool _hostRestart = false;
  List<int> get _delays =>
      _hostRestart ? _hostRestartBackoffSeconds : backoffSeconds;
  int get attemptLimit => _delays.length;

  int attempt = 0;
  bool waiting = false;

  /// Reserves the next attempt and returns its delay. Null means the ladder
  /// is exhausted; [waiting] is cleared so the page shows the normal
  /// disconnected card instead of a spinner that will never advance.
  Duration? next() {
    if (attempt >= attemptLimit) {
      waiting = false;
      return null;
    }
    attempt += 1;
    waiting = true;
    return Duration(seconds: _delays[attempt - 1]);
  }

  /// Lookup can fail before a native session exists to report a disconnect.
  /// Continue only an already active series, and never reset its attempt cap.
  Duration? afterStartupFailure({required bool retryable}) {
    if (!retryable) {
      reset();
      return null;
    }
    return waiting && attempt > 0 ? next() : null;
  }

  void reset({bool hostRestart = false}) {
    _hostRestart = hostRestart;
    attempt = 0;
    waiting = false;
  }
}
