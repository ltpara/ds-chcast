import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/annotation_stroke_controller.dart';

class _Sent {
  _Sent(this.action, this.strokeId, List<ViewerAnnotationPoint> points)
    : points = List<ViewerAnnotationPoint>.of(points);

  final int action;
  final int strokeId;
  final List<ViewerAnnotationPoint> points;
}

class _Sender {
  final List<_Sent> sent = <_Sent>[];
  bool throwNext = false;

  Future<bool> call(
    int action,
    int strokeId,
    List<ViewerAnnotationPoint> points,
  ) async {
    sent.add(_Sent(action, strokeId, points));
    if (throwNext) {
      throwNext = false;
      throw StateError('transport unavailable');
    }
    return true;
  }
}

void main() {
  late _Sender sender;
  late AnnotationStrokeController controller;

  setUp(() {
    sender = _Sender();
    controller = AnnotationStrokeController(sender: sender.call);
  });

  test('begin is sent immediately with a new non-zero stroke id', () async {
    await controller.begin(const ViewerAnnotationPoint(10, 20), nowMs: 1000);

    expect(sender.sent, hasLength(1));
    expect(sender.sent.single.action, AnnotationWireAction.begin);
    expect(sender.sent.single.strokeId, isNonZero);
    expect(sender.sent.single.points, <ViewerAnnotationPoint>[
      const ViewerAnnotationPoint(10, 20),
    ]);
    expect(controller.isDrawing, isTrue);
    expect(controller.canUndo, isTrue);
  });

  test('moves are batched until the throttle deadline', () async {
    await controller.begin(const ViewerAnnotationPoint(0, 0), nowMs: 1000);
    await controller.move(const ViewerAnnotationPoint(1, 1), nowMs: 1010);
    await controller.move(const ViewerAnnotationPoint(2, 2), nowMs: 1032);

    expect(sender.sent, hasLength(1));

    await controller.move(const ViewerAnnotationPoint(3, 3), nowMs: 1033);

    expect(sender.sent, hasLength(2));
    expect(sender.sent.last.action, AnnotationWireAction.move);
    expect(sender.sent.last.strokeId, sender.sent.first.strokeId);
    expect(sender.sent.last.points, <ViewerAnnotationPoint>[
      const ViewerAnnotationPoint(1, 1),
      const ViewerAnnotationPoint(2, 2),
      const ViewerAnnotationPoint(3, 3),
    ]);
  });

  test('64 pending moves force a bounded batch without waiting', () async {
    await controller.begin(const ViewerAnnotationPoint(0, 0), nowMs: 0);
    for (int i = 1; i <= 64; ++i) {
      await controller.move(ViewerAnnotationPoint(i, i), nowMs: 0);
    }

    expect(sender.sent, hasLength(2));
    expect(sender.sent.last.action, AnnotationWireAction.move);
    expect(sender.sent.last.points, hasLength(64));
    expect(sender.sent.last.points.last, const ViewerAnnotationPoint(64, 64));
  });

  test('end flushes the remaining points as the final move batch', () async {
    await controller.begin(const ViewerAnnotationPoint(0, 0), nowMs: 1000);
    await controller.move(const ViewerAnnotationPoint(1, 1), nowMs: 1010);
    await controller.move(const ViewerAnnotationPoint(2, 2), nowMs: 1020);

    await controller.end(point: const ViewerAnnotationPoint(3, 3), nowMs: 1030);

    expect(sender.sent.map((_Sent item) => item.action), <int>[
      AnnotationWireAction.begin,
      AnnotationWireAction.end,
    ]);
    expect(sender.sent.last.points, <ViewerAnnotationPoint>[
      const ViewerAnnotationPoint(1, 1),
      const ViewerAnnotationPoint(2, 2),
      const ViewerAnnotationPoint(3, 3),
    ]);
    expect(controller.isDrawing, isFalse);
  });

  test('a tap end still carries its only point', () async {
    await controller.begin(const ViewerAnnotationPoint(7, 9), nowMs: 1000);
    await controller.end(nowMs: 1010);

    expect(sender.sent.last.action, AnnotationWireAction.end);
    expect(sender.sent.last.strokeId, sender.sent.first.strokeId);
    expect(sender.sent.last.points, <ViewerAnnotationPoint>[
      const ViewerAnnotationPoint(7, 9),
    ]);
  });

  test(
    'undo ends an active stroke and uses the action-only wire shape',
    () async {
      await controller.begin(const ViewerAnnotationPoint(1, 1), nowMs: 0);

      expect(await controller.undo(), isTrue);

      expect(sender.sent.map((_Sent item) => item.action), <int>[
        AnnotationWireAction.begin,
        AnnotationWireAction.end,
        AnnotationWireAction.undo,
      ]);
      expect(sender.sent.last.strokeId, 0);
      expect(sender.sent.last.points, isEmpty);
      expect(controller.canUndo, isFalse);
      expect(await controller.undo(), isFalse);
      expect(sender.sent, hasLength(3));
    },
  );

  test('clear sends the action-only shape and resets undo history', () async {
    await controller.begin(const ViewerAnnotationPoint(1, 2), nowMs: 0);
    await controller.end(nowMs: 1);

    await controller.clear();

    expect(sender.sent.last.action, AnnotationWireAction.clear);
    expect(sender.sent.last.strokeId, 0);
    expect(sender.sent.last.points, isEmpty);
    expect(controller.canUndo, isFalse);
    expect(controller.isDrawing, isFalse);
  });

  test('separate controllers never allocate the same stroke id', () async {
    final _Sender secondSender = _Sender();
    final AnnotationStrokeController second = AnnotationStrokeController(
      sender: secondSender.call,
    );

    await controller.begin(const ViewerAnnotationPoint(1, 1), nowMs: 0);
    await second.begin(const ViewerAnnotationPoint(2, 2), nowMs: 0);

    expect(
      sender.sent.single.strokeId,
      isNot(secondSender.sent.single.strokeId),
    );
  });

  test(
    'a sender exception is contained and the next stroke recovers',
    () async {
      sender.throwNext = true;
      await controller.begin(const ViewerAnnotationPoint(1, 1), nowMs: 0);

      expect(controller.isDrawing, isTrue);
      await controller.end(nowMs: 1);
      expect(controller.isDrawing, isFalse);

      await controller.begin(const ViewerAnnotationPoint(3, 3), nowMs: 2);
      await controller.end(nowMs: 3);

      expect(sender.sent.last.action, AnnotationWireAction.end);
      expect(sender.sent.last.strokeId, isNonZero);
      expect(sender.sent.map((_Sent item) => item.action), <int>[
        AnnotationWireAction.begin,
        AnnotationWireAction.end,
        AnnotationWireAction.begin,
        AnnotationWireAction.end,
      ]);
      expect(controller.isDrawing, isFalse);
    },
  );
}
