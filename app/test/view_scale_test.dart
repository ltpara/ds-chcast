import 'package:chast_viewer/view_scale.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('view zoom slider preserves every supported integer percentage', () {
    for (
      int percent = minViewZoomPercent;
      percent <= maxViewZoomPercent;
      percent++
    ) {
      final double position = scalePercentToSlider(
        percent,
        min: minViewZoomPercent,
        pivot: defaultViewZoomPercent,
        max: maxViewZoomPercent,
        pivotPosition: viewZoomPivotPosition,
      );
      expect(
        scaleSliderToPercent(
          position,
          min: minViewZoomPercent,
          pivot: defaultViewZoomPercent,
          max: maxViewZoomPercent,
          pivotPosition: viewZoomPivotPosition,
        ),
        percent,
      );
    }
  });

  test('cursor zoom slider preserves its neutral size and bounds', () {
    for (final int percent in <int>[
      minCursorZoomPercent,
      defaultCursorZoomPercent,
      maxCursorZoomPercent,
    ]) {
      final double position = scalePercentToSlider(
        percent,
        min: minCursorZoomPercent,
        pivot: defaultCursorZoomPercent,
        max: maxCursorZoomPercent,
        pivotPosition: cursorZoomPivotPosition,
      );
      expect(
        scaleSliderToPercent(
          position,
          min: minCursorZoomPercent,
          pivot: defaultCursorZoomPercent,
          max: maxCursorZoomPercent,
          pivotPosition: cursorZoomPivotPosition,
        ),
        percent,
      );
    }
  });
}
