const int minViewZoomPercent = 5;
const int maxViewZoomPercent = 1000;
const int minCursorZoomPercent = 25;
const int maxCursorZoomPercent = 400;
const int defaultViewZoomPercent = 100;
const int defaultCursorZoomPercent = 100;

int normalizeScalePercent(Object? value, {required int min, required int max}) {
  if (value is! int) return defaultViewZoomPercent;
  return value.clamp(min, max).toInt();
}

double scalePercentToSlider(
  int percent, {
  required int min,
  required int pivot,
  required int max,
  required double pivotPosition,
}) {
  final int value = percent.clamp(min, max);
  if (value <= pivot) {
    return (value - min) / (pivot - min) * pivotPosition;
  }
  return pivotPosition + (value - pivot) / (max - pivot) * (1 - pivotPosition);
}

int scaleSliderToPercent(
  double position, {
  required int min,
  required int pivot,
  required int max,
  required double pivotPosition,
}) {
  final double value = position.clamp(0.0, 1.0).toDouble();
  if ((value - pivotPosition).abs() < 0.00001) return pivot;
  final double percent = value < pivotPosition
      ? min + (pivot - min) * value / pivotPosition
      : pivot + (max - pivot) * (value - pivotPosition) / (1 - pivotPosition);
  return percent.round().clamp(min, max).toInt();
}

int get viewZoomSliderDivisions => maxViewZoomPercent - minViewZoomPercent;
int get cursorZoomSliderDivisions =>
    maxCursorZoomPercent - minCursorZoomPercent;

double get viewZoomPivotPosition => 1 / 3;
double get cursorZoomPivotPosition => 1 / 2;
