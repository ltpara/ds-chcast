import 'dart:async';

/// Wire values for protocol v76 freehand annotations.
abstract final class AnnotationWireAction {
  static const int clear = 0;
  static const int begin = 1;
  static const int move = 2;
  static const int end = 3;
  static const int undo = 4;
}

class ViewerAnnotationPoint {
  const ViewerAnnotationPoint(this.x, this.y);

  final int x;
  final int y;

  @override
  bool operator ==(Object other) =>
      other is ViewerAnnotationPoint && other.x == x && other.y == y;

  @override
  int get hashCode => Object.hash(x, y);

  Map<String, int> toMap() => <String, int>{'x': x, 'y': y};
}

typedef AnnotationSender =
    Future<bool> Function(
      int action,
      int strokeId,
      List<ViewerAnnotationPoint> points,
    );

/// Turns a pointer drag into an ordered, bounded annotation stream.
///
/// Begin is sent immediately. Moves are batched at about 30 Hz and at 64
/// points, and End is always flushed on pointer-up or cancellation. The host
/// only needs the visual result, so there is no acknowledgement protocol; a
/// rejected send is ignored until the next stroke or clear.
class AnnotationStrokeController {
  factory AnnotationStrokeController({
    required AnnotationSender sender,
    int throttleMs = 33,
    int Function()? nowMs,
  }) => AnnotationStrokeController._(
    sender,
    throttleMs: throttleMs,
    nowMs: nowMs,
  );

  AnnotationStrokeController._(
    this._sender, {
    this.throttleMs = 33,
    int Function()? nowMs,
  }) : _nowMs = nowMs ?? (() => DateTime.now().millisecondsSinceEpoch);

  static const int maxBatchPoints = 64;

  final AnnotationSender _sender;
  final int Function() _nowMs;
  final int throttleMs;
  final List<ViewerAnnotationPoint> _pending = <ViewerAnnotationPoint>[];

  // Every session engine draws on the same host desktop. A counter per
  // controller would let the main session and an independent display window
  // allocate the same active stroke id, so moves from one window could be
  // appended to the other window's stroke.
  static int _nextGlobalStrokeId = 1;
  int? _activeStrokeId;
  ViewerAnnotationPoint? _lastPoint;
  int _lastFlushMs = 0;
  int _strokeCount = 0;

  bool get isDrawing => _activeStrokeId != null;
  bool get canUndo => _strokeCount > 0;

  static int _allocateStrokeId() {
    final int id = _nextGlobalStrokeId;
    _nextGlobalStrokeId = id == 0xffffffff ? 1 : id + 1;
    return id;
  }

  Future<void> begin(ViewerAnnotationPoint point, {int? nowMs}) async {
    if (_activeStrokeId != null) {
      await end(nowMs: nowMs);
    }
    final int strokeId = _allocateStrokeId();
    final int now = nowMs ?? _nowMs();
    _activeStrokeId = strokeId;
    _lastPoint = point;
    _lastFlushMs = now;
    _pending.clear();
    _strokeCount++;
    await _send(AnnotationWireAction.begin, strokeId, <ViewerAnnotationPoint>[
      point,
    ]);
  }

  Future<void> move(ViewerAnnotationPoint point, {int? nowMs}) async {
    if (_activeStrokeId == null || point == _lastPoint) {
      return;
    }
    _lastPoint = point;
    _pending.add(point);
    final int now = nowMs ?? _nowMs();
    if (_pending.length >= maxBatchPoints || now - _lastFlushMs >= throttleMs) {
      await _flushMoves(now);
    }
  }

  Future<void> end({ViewerAnnotationPoint? point, int? nowMs}) async {
    final int? strokeId = _activeStrokeId;
    if (strokeId == null) {
      return;
    }
    if (point != null && point != _lastPoint) {
      _lastPoint = point;
      _pending.add(point);
    }
    final int now = nowMs ?? _nowMs();
    while (_pending.length > maxBatchPoints) {
      final List<ViewerAnnotationPoint> batch = _pending.sublist(
        0,
        maxBatchPoints,
      );
      _pending.removeRange(0, maxBatchPoints);
      await _send(AnnotationWireAction.move, strokeId, batch);
    }
    if (_pending.isEmpty) {
      // Protocol End still carries one point. Repeating the final position is
      // harmless and lets the host close a tap as a dot.
      _pending.add(_lastPoint!);
    }
    final List<ViewerAnnotationPoint> finalBatch =
        List<ViewerAnnotationPoint>.of(_pending);
    _pending.clear();
    _activeStrokeId = null;
    _lastPoint = null;
    _lastFlushMs = now;
    await _send(AnnotationWireAction.end, strokeId, finalBatch);
  }

  Future<void> flush({int? nowMs}) => end(nowMs: nowMs);

  Future<bool> undo() async {
    if (_activeStrokeId != null) {
      await end();
    }
    if (_strokeCount == 0) {
      return false;
    }
    _strokeCount--;
    return _send(AnnotationWireAction.undo, 0, const <ViewerAnnotationPoint>[]);
  }

  Future<void> clear() async {
    if (_activeStrokeId != null) {
      await end();
    }
    _strokeCount = 0;
    await _send(AnnotationWireAction.clear, 0, const <ViewerAnnotationPoint>[]);
  }

  Future<void> _flushMoves(int now) async {
    final int? strokeId = _activeStrokeId;
    if (strokeId == null || _pending.isEmpty) {
      return;
    }
    final List<ViewerAnnotationPoint> batch = List<ViewerAnnotationPoint>.of(
      _pending,
    );
    _pending.clear();
    _lastFlushMs = now;
    await _send(AnnotationWireAction.move, strokeId, batch);
  }

  Future<bool> _send(
    int action,
    int strokeId,
    List<ViewerAnnotationPoint> points,
  ) async {
    try {
      return await _sender(action, strokeId, points);
    } catch (_) {
      return false;
    }
  }
}

/// Fire-and-forget helper for pointer callbacks, which cannot await.
void runAnnotationTask(Future<void> task) {
  unawaited(task);
}
