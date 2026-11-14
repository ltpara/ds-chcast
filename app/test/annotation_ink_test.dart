import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/annotation_ink.dart';

void main() {
  test('a stroke records its surface and ordered points', () {
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);

    ink.begin(7, const Offset(10, 20));
    ink.move(const Offset(11, 21));
    ink.move(const Offset(11, 21));
    ink.move(const Offset(12, 22));
    ink.end();

    expect(ink.strokes, hasLength(1));
    final ViewerInkStroke stroke = ink.strokes.single;
    expect(stroke.surface, 7);
    expect(stroke.points, const <Offset>[
      Offset(10, 20),
      Offset(11, 21),
      Offset(12, 22),
    ]);
    expect(ink.isDrawing, isFalse);
  });

  test('moves without an active stroke and repeated points are ignored', () {
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);

    ink.move(const Offset(1, 1));
    expect(ink.strokes, isEmpty);

    ink.begin(0, const Offset(5, 5));
    final int revision = ink.revision;
    ink.move(const Offset(5, 5));
    expect(ink.revision, revision, reason: 'a repeated point is not a change');
  });

  test('a new contact closes the previous stroke', () {
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);

    ink.begin(0, const Offset(1, 1));
    ink.move(const Offset(2, 2));
    ink.begin(0, const Offset(30, 30));

    expect(ink.strokes, hasLength(2));
    expect(ink.strokes.first.points, const <Offset>[
      Offset(1, 1),
      Offset(2, 2),
    ]);
    expect(ink.strokes.last.points, const <Offset>[Offset(30, 30)]);
    expect(ink.isDrawing, isTrue);
  });

  test('undo finishes the active stroke and drops the newest one', () {
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);

    ink.begin(0, const Offset(1, 1));
    ink.end();
    ink.begin(0, const Offset(2, 2));
    ink.undo();

    expect(ink.isDrawing, isFalse);
    expect(ink.strokes, hasLength(1));
    expect(ink.strokes.single.points, const <Offset>[Offset(1, 1)]);

    ink.undo();
    expect(ink.strokes, isEmpty);
  });

  test('clear drops every stroke', () {
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);

    ink.begin(0, const Offset(1, 1));
    ink.end();
    ink.begin(3, const Offset(2, 2));
    ink.clear();

    expect(ink.strokes, isEmpty);
    expect(ink.isDrawing, isFalse);
  });

  test('history is bounded like the host overlay', () {
    final ViewerAnnotationInk ink = ViewerAnnotationInk();
    addTearDown(ink.dispose);

    for (int i = 0; i <= ViewerAnnotationInk.maxStrokes; ++i) {
      ink.begin(0, Offset(i.toDouble(), 0));
    }
    expect(ink.strokes, hasLength(ViewerAnnotationInk.maxStrokes));
    expect(
      ink.strokes.first.points.single,
      Offset(1, 0),
      reason: 'the oldest stroke is evicted first',
    );

    ink.begin(0, Offset.zero);
    for (int i = 0; i <= ViewerAnnotationInk.maxPointsPerStroke; ++i) {
      ink.move(Offset(i.toDouble(), 1));
    }
    expect(
      ink.strokes.last.points,
      hasLength(ViewerAnnotationInk.maxPointsPerStroke),
    );
    expect(
      ink.strokes.last.points.last,
      Offset(ViewerAnnotationInk.maxPointsPerStroke.toDouble(), 1),
      reason: 'an overlong stroke keeps its newest tail',
    );
  });
}