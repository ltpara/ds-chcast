import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/rendering.dart';

/// One locally drawn annotation stroke in the frame coordinates of the
/// surface that drew it.
class ViewerInkStroke {
  ViewerInkStroke(this.surface, Offset point) : points = <Offset>[point];

  /// Identifies the picture the points belong to: a display stream id, or 0
  /// for the single captured desktop.
  final int surface;
  final List<Offset> points;
}

/// Local echo of the strokes this engine drew.
///
/// The host paints annotations on a capture-excluded overlay, so the encoded
/// video never contains them and a viewer would otherwise draw blind. This
/// model mirrors only this engine's own strokes; the wire protocol has no
/// host-to-viewer annotation echo, so strokes drawn from another window are
/// not shown here. The bounds match the host history (2048 strokes and 65536
/// points across all of them, oldest evicted first), so the local copy cannot
/// outlive or outgrow what the host still keeps.
class ViewerAnnotationInk extends ChangeNotifier {
  static const int maxStrokes = 2048;
  static const int maxPoints = 65536;

  final List<ViewerInkStroke> _strokes = <ViewerInkStroke>[];
  ViewerInkStroke? _active;
  int _pointCount = 0;
  int _revision = 0;

  List<ViewerInkStroke> get strokes => List<ViewerInkStroke>.unmodifiable(
    _strokes,
  );
  bool get isDrawing => _active != null;
  bool get isEmpty => _strokes.isEmpty && _active == null;

  /// Bumped on every change so the painter can skip unchanged frames.
  int get revision => _revision;

  void _changed() {
    _revision++;
    notifyListeners();
  }

  void begin(int surface, Offset point) {
    end();
    final ViewerInkStroke stroke = ViewerInkStroke(surface, point);
    _strokes.add(stroke);
    _pointCount++;
    _active = stroke;
    _trim();
    _changed();
  }

  void move(Offset point) {
    final ViewerInkStroke? stroke = _active;
    if (stroke == null || stroke.points.last == point) {
      return;
    }
    stroke.points.add(point);
    _pointCount++;
    _trim();
    _changed();
  }

  void end() {
    if (_active == null) {
      return;
    }
    _active = null;
    _changed();
  }

  /// Mirrors AnnotationStrokeController.undo(): an active stroke is finished
  /// first, then the newest stroke is dropped.
  void undo() {
    end();
    if (_strokes.isEmpty) {
      return;
    }
    _pointCount -= _strokes.removeLast().points.length;
    _changed();
  }

  void clear() {
    _active = null;
    _strokes.clear();
    _pointCount = 0;
    _changed();
  }

  /// Mirrors detail::AnnotationHistory::trim: drop whole strokes oldest first,
  /// then trim the oldest stroke's head points when it alone is over budget.
  void _trim() {
    while (_strokes.length > maxStrokes) {
      _removeOldest();
    }
    while (_pointCount > maxPoints && _strokes.length > 1) {
      _removeOldest();
    }
    if (_pointCount > maxPoints) {
      final List<Offset> points = _strokes.first.points;
      final int excess = math.min(_pointCount - maxPoints, points.length - 1);
      if (excess > 0) {
        points.removeRange(0, excess);
        _pointCount -= excess;
      }
    }
  }

  void _removeOldest() {
    final ViewerInkStroke oldest = _strokes.removeAt(0);
    _pointCount -= oldest.points.length;
    if (identical(_active, oldest)) {
      // The host deactivates an evicted stroke, so later moves are dropped.
      _active = null;
    }
  }
}

/// Paints [ViewerAnnotationInk] over the video surface.
///
/// Points are captured-desktop pixels, so they scale exactly like the frame.
/// The style approximates the host overlay: a 4.5 px radius at 96 dpi with a
/// dark rim for light wallpapers. The host derives the colour from the viewer
/// label, which the viewer cannot see, so the local echo uses the palette's
/// warm red directly.
class ViewerInkPainter extends CustomPainter {
  ViewerInkPainter({
    required this.ink,
    required this.surface,
    required this.sourceWidth,
    required this.sourceHeight,
  }) : revision = ink.revision;

  final ViewerAnnotationInk ink;
  final int surface;
  final double sourceWidth;
  final double sourceHeight;

  /// Snapshot of [ViewerAnnotationInk.revision] when this painter was built;
  /// the model mutates in place, so the painter must remember the version.
  final int revision;

  static const Color strokeColor = Color(0xE1E4572E);
  static const Color shadowColor = Color(0x33000000);
  static const double strokeRadius = 4.5;

  static Offset _map(Offset point, double sx, double sy) =>
      Offset(point.dx * sx, point.dy * sy);

  @override
  void paint(Canvas canvas, Size size) {
    if (sourceWidth <= 0 || sourceHeight <= 0) {
      return;
    }
    final double sx = size.width / sourceWidth;
    final double sy = size.height / sourceHeight;
    if (!sx.isFinite || !sy.isFinite || sx <= 0 || sy <= 0) {
      return;
    }
    final double width = math.max(
      2.0,
      strokeRadius * 2 * math.min(sx, sy),
    );
    final Paint shadow = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = width + 2
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round
      ..color = shadowColor;
    final Paint line = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = width
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round
      ..color = strokeColor;
    final Paint dot = Paint()..color = strokeColor;
    for (final ViewerInkStroke stroke in ink.strokes) {
      if (stroke.surface != surface || stroke.points.isEmpty) {
        continue;
      }
      final List<Offset> points = stroke.points;
      if (points.length == 1) {
        canvas.drawCircle(_map(points.first, sx, sy), width / 2, dot);
        continue;
      }
      final Offset start = _map(points.first, sx, sy);
      final Path path = Path()..moveTo(start.dx, start.dy);
      for (int i = 1; i < points.length; ++i) {
        final Offset next = _map(points[i], sx, sy);
        path.lineTo(next.dx, next.dy);
      }
      canvas.drawPath(path, shadow);
      canvas.drawPath(path, line);
    }
  }

  @override
  bool shouldRepaint(ViewerInkPainter oldDelegate) =>
      oldDelegate.ink != ink ||
      oldDelegate.surface != surface ||
      oldDelegate.sourceWidth != sourceWidth ||
      oldDelegate.sourceHeight != sourceHeight ||
      oldDelegate.revision != revision;
}