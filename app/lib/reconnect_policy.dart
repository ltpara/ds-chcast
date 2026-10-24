/// The backoff ladder for an unexpected viewer drop.
///
/// The page decides when a drop qualifies (a frame was seen, no denial, no
/// identity change) and asks for one attempt at a time. Keeping the ladder
/// here makes the schedule and the five-attempt cap testable without a
/// native session.
class AutoReconnectPolicy {
  static const List<int> backoffSeconds = <int>[1, 2, 4, 8, 15];
  static const int limit = 5;

  int attempt = 0;
  bool waiting = false;

  /// Reserves the next attempt and returns its delay. Null means the ladder
  /// is exhausted; [waiting] is cleared so the page shows the normal
  /// disconnected card instead of a spinner that will never advance.
  Duration? next() {
    if (attempt >= limit) {
      waiting = false;
      return null;
    }
    attempt += 1;
    waiting = true;
    return Duration(seconds: backoffSeconds[attempt - 1]);
  }

  void reset() {
    attempt = 0;
    waiting = false;
  }
}
