import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/widgets/remote_view.dart';

void main() {
  test('maps a primary-display position from frame to desktop pixels', () {
    final (int x, int y) = frameToDesktop(
      const Offset(10.4, 20.6),
      originX: 0,
      originY: 0,
      width: 1920,
      height: 1080,
    );
    expect(x, 10);
    expect(y, 21);
  });

  test('adds a negative origin for a display left of the primary', () {
    final (int x, int y) = frameToDesktop(
      const Offset(40, 30),
      originX: -1080,
      originY: 0,
      width: 1080,
      height: 1920,
    );
    expect(x, -1040);
    expect(y, 30);
  });

  test('adds a positive origin for a display right of the primary', () {
    final (int x, int y) = frameToDesktop(
      const Offset(100, 50),
      originX: 1920,
      originY: 0,
      width: 2560,
      height: 1440,
    );
    expect(x, 2020);
    expect(y, 50);
  });

  test('clamps to the display rect instead of a neighbouring monitor', () {
    final (int leftX, int leftY) = frameToDesktop(
      const Offset(-5, -4),
      originX: -1080,
      originY: 0,
      width: 1080,
      height: 1920,
    );
    expect(leftX, -1080);
    expect(leftY, 0);

    final (int rightX, int rightY) = frameToDesktop(
      const Offset(1080, 1920),
      originX: -1080,
      originY: 0,
      width: 1080,
      height: 1920,
    );
    expect(rightX, -1);
    expect(rightY, 1919);

    final (int topX, int topY) = frameToDesktop(
      const Offset(5000, 5000),
      originX: 0,
      originY: 0,
      width: 1920,
      height: 1080,
    );
    expect(topX, 1919);
    expect(topY, 1079);
  });

  test('rounds half pixels away from zero like the old page math', () {
    final (int x, int y) = frameToDesktop(
      const Offset(10.5, 20.5),
      originX: 0,
      originY: 0,
      width: 100,
      height: 100,
    );
    expect(x, 11);
    expect(y, 21);
  });

  test('falls back to a 1x1 rect when the source size is unknown', () {
    final (int x, int y) = frameToDesktop(
      const Offset(3, 4),
      originX: 7,
      originY: 9,
      width: 0,
      height: 0,
    );
    expect(x, 7);
    expect(y, 9);
  });

  test('a display switch remaps the same frame to the new origin and size', () {
    const Offset frame = Offset(100, 50);

    final (int primaryX, int primaryY) = frameToDesktop(
      frame,
      originX: 0,
      originY: 0,
      width: 1920,
      height: 1080,
    );
    expect(primaryX, 100);
    expect(primaryY, 50);

    final (int rightX, int rightY) = frameToDesktop(
      frame,
      originX: 1920,
      originY: 0,
      width: 2560,
      height: 1440,
    );
    expect(rightX, 2020);
    expect(rightY, 50);

    final (int leftX, int leftY) = frameToDesktop(
      frame,
      originX: -1080,
      originY: 0,
      width: 1080,
      height: 1920,
    );
    expect(leftX, -980);
    expect(leftY, 50);
  });
}
