import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

import '../annotation_ink.dart';
import '../trackpad_speed.dart';
import '../view_scroll.dart';
import '../viewer_bridge.dart';

enum ViewFit { adaptive, original }

/// Pointer callbacks report positions in captured-desktop (source) pixels:
/// (0, 0) is the desktop's top-left, before the virtual-desktop origin is
/// added. The host may encode a smaller picture than the desktop, so these are
/// source pixels, not texture pixels; the session page adds the origin last.
typedef FramePointerHandler = void Function(Offset position, int buttons);
typedef FrameButtonHandler =
    void Function(Offset position, int button, bool down);
typedef FrameScrollHandler = void Function(Offset position, Offset delta);

/// Precision-touchpad pan or pinch. Deltas are WHEEL_DELTA units (120 per
/// notch); zoom uses only [deltaY] and is a complete Ctrl+wheel chord.
typedef FrameGestureHandler =
    void Function(Offset position, bool zoom, int deltaX, int deltaY);

/// Touch and stylus events, which carry pressure and tilt the mouse path does
/// not model.
typedef FrameDeviceHandler = void Function(PointerEvent event, Offset position);

const int _mouseButtonMask =
    ViewerMouse.left |
    ViewerMouse.right |
    ViewerMouse.middle |
    ViewerMouse.back |
    ViewerMouse.forward;
const List<int> _mouseButtons = <int>[
  ViewerMouse.left,
  ViewerMouse.right,
  ViewerMouse.middle,
  ViewerMouse.back,
  ViewerMouse.forward,
];

/// Maps a position inside the drawn frame to virtual-desktop pixels for the
/// announced display. The frame's (0, 0) is the display's top-left, so the
/// display origin is added here. Clamping to the display rect keeps a press
/// on the frame edge, or a rounding overshoot, from landing on a neighbouring
/// monitor in a multi-display layout. A missing source size falls back to a
/// 1x1 rect, matching the session page's empty-status defaults.
(int x, int y) frameToDesktop(
  Offset frame, {
  required int originX,
  required int originY,
  required int width,
  required int height,
}) {
  final int w = width > 0 ? width : 1;
  final int h = height > 0 ? height : 1;
  final int x = (originX + frame.dx)
      .round()
      .clamp(originX, originX + w - 1)
      .toInt();
  final int y = (originY + frame.dy)
      .round()
      .clamp(originY, originY + h - 1)
      .toInt();
  return (x, y);
}

class RemoteView extends StatefulWidget {
  const RemoteView({
    super.key,
    required this.textureId,
    required this.videoWidth,
    required this.videoHeight,
    this.sourceWidth = 0,
    this.sourceHeight = 0,
    required this.fit,
    this.zoomPercent = 100,
    this.trackpadSpeedPercent = kDefaultTrackpadSpeedPercent,
    this.scrollStyle = defaultViewScrollStyle,
    this.scrollEdge = defaultViewScrollEdge,
    this.pointerPanEnabled = true,
    this.captureInput = false,
    this.drawEnabled = false,
    this.hideCursor = false,
    this.onScaleChanged,
    this.onHover,
    this.onMove,
    this.onButton,
    this.onScroll,
    this.onGesture,
    this.onDevicePointer,
    this.onDeviceCancel,
    this.onDrawStart,
    this.onDrawMove,
    this.onDrawEnd,
    this.onDrawCancel,
    this.onCursorHint,
    this.onCursorHintPress,
    this.onCursorHintExit,
    this.onFocusRequested,
    this.overlayBuilder,
    this.ink,
    this.inkSurface = 0,
  });

  final int textureId;

  /// The encoded picture size, which is what the texture holds and what the
  /// aspect ratio and drawing size follow.
  final int videoWidth;
  final int videoHeight;

  /// The captured desktop size, which is the space the host sends pointer
  /// positions in. Zero falls back to the video size.
  final int sourceWidth;
  final int sourceHeight;
  final ViewFit fit;
  final int zoomPercent;
  final int trackpadSpeedPercent;
  final ViewScrollStyle scrollStyle;
  final int scrollEdge;
  final bool pointerPanEnabled;

  int get _pointerWidth => sourceWidth > 0 ? sourceWidth : videoWidth;
  int get _pointerHeight => sourceHeight > 0 ? sourceHeight : videoHeight;

  /// When true the view forwards raw pointer events. The session page only
  /// enables this while remote control is on.
  final bool captureInput;

  /// While true, primary drags draw temporary host annotations instead of
  /// forwarding input. This branch deliberately bypasses every input callback
  /// so a gesture can never also reach the remote mouse injector.
  final bool drawEnabled;

  /// True while the native side has installed the host's own pointer. The
  /// local one is hidden only then, so a host that sends no sprite (a machine
  /// with no pointing device) still leaves the user an arrow to aim with.
  final bool hideCursor;

  /// Reports the drawn size in logical pixels per video pixel, which is what
  /// the remote pointer is scaled by. Called after layout, only on change.
  final ValueChanged<double>? onScaleChanged;
  final FramePointerHandler? onHover;
  final FramePointerHandler? onMove;
  final FrameButtonHandler? onButton;
  final FrameScrollHandler? onScroll;
  final FrameGestureHandler? onGesture;

  /// Receives every touch or stylus down/move/up; [onDeviceCancel] receives a
  /// cancelled contact. Hover events are not forwarded because a floating pen
  /// has no contact to inject.
  final FrameDeviceHandler? onDevicePointer;
  final FrameDeviceHandler? onDeviceCancel;
  final ValueChanged<Offset>? onDrawStart;
  final ValueChanged<Offset>? onDrawMove;
  final ValueChanged<Offset>? onDrawEnd;
  final VoidCallback? onDrawCancel;

  /// Reports the pointer position in captured-desktop pixels for the host's
  /// "show my cursor" hint. Unlike the input callbacks it also runs while
  /// remote control is off, because the hint is draw-only. [onCursorHintExit]
  /// fires when the pointer leaves the picture so the host can hide it.
  final ValueChanged<Offset>? onCursorHint;

  /// The primary-button press edge at the given position. It is sent ahead of
  /// the throttled moves so the host overlay can ripple at the click instead
  /// of at the next move sample.
  final ValueChanged<Offset>? onCursorHintPress;
  final VoidCallback? onCursorHintExit;
  final VoidCallback? onFocusRequested;
  final Widget Function(Size drawn)? overlayBuilder;

  /// Local echo of this engine's annotation strokes. The host paints them on a
  /// capture-excluded overlay, so the video never shows them; while draw mode
  /// is on this layer gives the operator immediate feedback. Only strokes for
  /// [inkSurface] are painted.
  final ViewerAnnotationInk? ink;
  final int inkSurface;

  @override
  State<RemoteView> createState() => _RemoteViewState();
}

class _RemoteViewState extends State<RemoteView>
    with SingleTickerProviderStateMixin, WidgetsBindingObserver {
  final ScrollController _vertical = ScrollController(keepScrollOffset: false);
  final ScrollController _horizontal = ScrollController(
    keepScrollOffset: false,
  );
  final GlobalKey _frameKey = GlobalKey();
  final GlobalKey _viewportKey = GlobalKey();
  late final Ticker _edgeTicker;
  Offset? _panPointer;
  Duration _lastTick = Duration.zero;
  bool _panReportQueued = false;

  @override
  void initState() {
    super.initState();
    _edgeTicker = createTicker(_tickEdge);
    WidgetsBinding.instance.addObserver(this);
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state != AppLifecycleState.resumed) _stopPointerPan();
  }

  /// Buttons currently down on the local side. Pointer events carry the whole
  /// state, while the protocol wants one button per message, so down/up are
  /// derived from the difference against this.
  int _buttons = 0;
  Offset _lastFrame = Offset.zero;
  double? _reportedScale;
  int? _panZoomPointer;
  double _panZoomScale = 1;
  double _gestureX = 0;
  double _gestureY = 0;
  double _gestureZoom = 0;
  bool _zooming = false;
  int? _drawPointer;
  Offset _lastDrawFrame = Offset.zero;

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _edgeTicker.dispose();
    _releaseButtons();
    if (_drawPointer != null) {
      widget.onDrawEnd?.call(_lastDrawFrame);
      _drawPointer = null;
    }
    _vertical.dispose();
    _horizontal.dispose();
    super.dispose();
  }

  /// Tells the page how large the frame is drawn, once per change. Deferred to
  /// after the frame because the page answers with a native call, and
  /// [build] must not have a side effect on the engine.
  void _reportScale(Size drawn) {
    if (widget._pointerWidth <= 0 || drawn.width <= 0) {
      return;
    }
    // The host's sprite is in source pixels, so a downscaled encode drawn at
    // full window size still gets a full-size pointer.
    final double scale = drawn.width / widget._pointerWidth;
    if (_reportedScale != null && (_reportedScale! - scale).abs() < 0.0005) {
      return;
    }
    _reportedScale = scale;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) {
        widget.onScaleChanged?.call(scale);
      }
    });
  }

  @override
  void didUpdateWidget(covariant RemoteView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.scrollStyle != widget.scrollStyle ||
        oldWidget.scrollEdge != widget.scrollEdge ||
        oldWidget.pointerPanEnabled != widget.pointerPanEnabled ||
        oldWidget.drawEnabled != widget.drawEnabled ||
        oldWidget.captureInput != widget.captureInput ||
        oldWidget.fit != widget.fit ||
        oldWidget.zoomPercent != widget.zoomPercent ||
        oldWidget.videoWidth != widget.videoWidth ||
        oldWidget.videoHeight != widget.videoHeight) {
      _stopPointerPan();
    }
    if (oldWidget.captureInput && !widget.captureInput) {
      _releaseButtons();
      _panZoomPointer = null;
      _resetGesture();
    }
    if (oldWidget.drawEnabled && !widget.drawEnabled && _drawPointer != null) {
      widget.onDrawEnd?.call(_lastDrawFrame);
      _drawPointer = null;
    }
  }

  Offset _toFrame(Offset local, Size box) {
    if (box.width <= 0 || box.height <= 0) {
      return Offset.zero;
    }
    final int width = widget._pointerWidth;
    final int height = widget._pointerHeight;
    final double x = (local.dx * width / box.width)
        .clamp(0.0, width - 1.0)
        .toDouble();
    final double y = (local.dy * height / box.height)
        .clamp(0.0, height - 1.0)
        .toDouble();
    return Offset(x, y);
  }

  void _releaseButtons() {
    if (_buttons == 0) {
      return;
    }
    final int held = _buttons;
    _buttons = 0;
    for (final int bit in _mouseButtons) {
      if (held & bit != 0) {
        widget.onButton?.call(_lastFrame, bit, false);
      }
    }
  }

  // A pressed pointer keeps the hit-test transform from its down event.
  // Reproject through the current frame when a local scroll moved the image.
  Offset _eventFrame(PointerEvent event, Size box) {
    final frame = _frameKey.currentContext?.findRenderObject();
    return _toFrame(
      frame is RenderBox
          ? frame.globalToLocal(event.position)
          : event.localPosition,
      box,
    );
  }

  double _scrollRange(ScrollController controller) =>
      controller.hasClients && controller.position.hasContentDimensions
      ? controller.position.maxScrollExtent
      : 0;

  bool _panTo(ScrollController controller, double target) {
    final range = _scrollRange(controller);
    if (range <= 0) return false;
    final value = target.clamp(0.0, range);
    if ((controller.offset - value).abs() < 0.01) return false;
    controller.jumpTo(value);
    return true;
  }

  bool _panBy(ScrollController controller, double delta) =>
      controller.hasClients &&
      delta != 0 &&
      _panTo(controller, controller.offset + delta);

  void _handlePointerSignal(PointerSignalEvent event, Size box) {
    if (event is! PointerScrollEvent) return;
    final shift = HardwareKeyboard.instance.isShiftPressed;
    final overflow =
        _scrollRange(_horizontal) > 0 || _scrollRange(_vertical) > 0;
    if ((!widget.captureInput || widget.drawEnabled) && !overflow) return;
    GestureBinding.instance.pointerSignalResolver.register(event, (_) {
      if (widget.captureInput && !widget.drawEnabled && !(shift && overflow)) {
        widget.onScroll?.call(_eventFrame(event, box), event.scrollDelta);
        return;
      }
      _stopPointerPan();
      final delta = event.scrollDelta;
      if (shift && delta.dx == 0 && _scrollRange(_horizontal) > 0) {
        _panBy(_horizontal, delta.dy);
      } else {
        _panBy(_horizontal, delta.dx);
        _panBy(_vertical, delta.dy);
      }
    });
  }

  void _stopPointerPan() {
    _panPointer = null;
    if (_edgeTicker.isActive) _edgeTicker.stop();
    _lastTick = Duration.zero;
  }

  (Offset, Size)? _pointerInViewport(Offset global) {
    final viewport = _viewportKey.currentContext?.findRenderObject();
    if (viewport is! RenderBox || !viewport.hasSize) return null;
    final local = viewport.globalToLocal(global);
    return (Offset.zero & viewport.size).contains(local)
        ? (local, viewport.size)
        : null;
  }

  double _edgeVelocity(double point, double extent) {
    final zone = math.min(
      normalizeViewScrollEdge(widget.scrollEdge).toDouble(),
      extent / 3,
    );
    if (point < zone) return -900 * (1 - point / zone);
    if (point > extent - zone) return 900 * (1 - (extent - point) / zone);
    return 0;
  }

  bool _canPan(ScrollController controller, double velocity) {
    final range = _scrollRange(controller);
    if (range <= 0) return false;
    return velocity < 0 && controller.offset > 0 ||
        velocity > 0 && controller.offset < range;
  }

  void _tickEdge(Duration elapsed) {
    final global = _panPointer;
    final sample = global == null ? null : _pointerInViewport(global);
    if (sample == null) {
      _stopPointerPan();
      return;
    }
    final (point, viewport) = sample;
    final dx = _edgeVelocity(point.dx, viewport.width);
    final dy = _edgeVelocity(point.dy, viewport.height);
    if (!_canPan(_horizontal, dx) && !_canPan(_vertical, dy)) {
      _stopPointerPan();
      return;
    }
    final seconds = (elapsed - _lastTick).inMicroseconds.clamp(0, 40000) / 1e6;
    _lastTick = elapsed;
    final x = _panBy(_horizontal, dx * seconds);
    final y = _panBy(_vertical, dy * seconds);
    if (x || y) _queuePanHover();
  }

  void _queuePanHover() {
    if (_panReportQueued) return;
    _panReportQueued = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _panReportQueued = false;
      final global = _panPointer;
      final frame = _frameKey.currentContext?.findRenderObject();
      if (!mounted || global == null || frame is! RenderBox || !frame.hasSize) {
        return;
      }
      final position = _toFrame(frame.globalToLocal(global), frame.size);
      _reportHover(position, 0);
    });
  }

  void _reportHover(Offset position, int buttons) {
    if (widget.captureInput && !widget.drawEnabled) {
      _syncButtons(buttons, position);
      widget.onHover?.call(position, buttons);
    }
    widget.onCursorHint?.call(position);
  }

  void _handleHover(PointerHoverEvent event, Size box) {
    if (_isDevicePointer(event)) return;
    if (widget.pointerPanEnabled &&
        !widget.drawEnabled &&
        event.buttons == 0 &&
        widget.scrollStyle != ViewScrollStyle.scrollbar) {
      final sample = _pointerInViewport(event.position);
      if (sample != null) {
        final (point, viewport) = sample;
        _panPointer = event.position;
        if (widget.scrollStyle == ViewScrollStyle.auto) {
          final x = _panTo(
            _horizontal,
            _scrollRange(_horizontal) * point.dx / viewport.width,
          );
          final y = _panTo(
            _vertical,
            _scrollRange(_vertical) * point.dy / viewport.height,
          );
          if (x || y) {
            _queuePanHover();
            return;
          }
        } else {
          final x = _edgeVelocity(point.dx, viewport.width);
          final y = _edgeVelocity(point.dy, viewport.height);
          if (_canPan(_horizontal, x) || _canPan(_vertical, y)) {
            if (!_edgeTicker.isActive) _edgeTicker.start();
          } else {
            _stopPointerPan();
          }
        }
      }
    }
    _reportHover(_eventFrame(event, box), event.buttons);
  }

  /// Reconciles the buttons this view believes are down with the bitmask the
  /// embedder reports. Flutter delivers a second button pressed while another
  /// is held as a move, and a PointerUpEvent whose buttons field already omits
  /// the released bit, so neither down nor up alone names the changed button.
  void _syncButtons(int buttons, Offset position) {
    _lastFrame = position;
    final int target = buttons & _mouseButtonMask;
    final int changed = _buttons ^ target;
    if (changed == 0) {
      return;
    }
    _buttons = target;
    for (final int bit in _mouseButtons) {
      if (changed & bit != 0) {
        widget.onButton?.call(position, bit, target & bit != 0);
      }
    }
  }

  bool _isDevicePointer(PointerEvent event) {
    return event.kind == PointerDeviceKind.touch ||
        event.kind == PointerDeviceKind.stylus ||
        event.kind == PointerDeviceKind.invertedStylus;
  }

  void _resetGesture() {
    _gestureX = _gestureY = _gestureZoom = 0;
    _panZoomScale = 1;
    _zooming = false;
  }

  void _gesture(Offset position, Offset pan, double scale) {
    if (!pan.dx.isFinite || !pan.dy.isFinite || !scale.isFinite || scale <= 0) {
      return;
    }
    _gestureZoom += math.log(scale.clamp(0.25, 4)) / math.log(1.1) * 120;
    // Ignore small span noise during a two-finger pan.
    if (_gestureZoom.abs() >= 60) _zooming = true;
    if (_zooming) {
      // Accumulate small pinch updates: roughly ten percent is one zoom step.
      // Once a gesture becomes a pinch, centroid drift must not also scroll.
      final int steps = (_gestureZoom / 120).truncate();
      if (steps != 0) {
        _gestureZoom -= steps * 120;
        widget.onGesture?.call(position, true, 0, steps.clamp(-10, 10) * 120);
      }
      return;
    }
    // Direct manipulation follows the fingers, the opposite of scrollDelta.
    final double speed =
        normalizeTrackpadSpeedPercent(widget.trackpadSpeedPercent) / 100.0;
    _gestureX -=
        pan.dx * speed * kViewerWheelDelta / kViewerScrollPixelsPerNotch;
    _gestureY +=
        pan.dy * speed * kViewerWheelDelta / kViewerScrollPixelsPerNotch;
    final int x = _gestureX.truncate();
    final int y = _gestureY.truncate();
    _gestureX -= x;
    _gestureY -= y;
    if (x != 0 || y != 0) {
      widget.onGesture?.call(
        position,
        false,
        x.clamp(-1200, 1200),
        y.clamp(-1200, 1200),
      );
    }
  }

  Widget _layer(Size box) {
    _reportScale(box);
    Widget texture = Texture(
      textureId: widget.textureId,
      filterQuality: widget.fit == ViewFit.original
          ? FilterQuality.none
          : FilterQuality.low,
    );
    if (widget.overlayBuilder != null) {
      texture = ClipRect(
        child: Stack(
          fit: StackFit.expand,
          children: [
            texture,
            IgnorePointer(child: widget.overlayBuilder!(box)),
          ],
        ),
      );
    }
    final ViewerAnnotationInk? ink = widget.ink;
    if (widget.drawEnabled && ink != null) {
      texture = ClipRect(
        child: Stack(
          fit: StackFit.expand,
          children: <Widget>[
            texture,
            IgnorePointer(
              child: ListenableBuilder(
                listenable: ink,
                builder: (BuildContext context, Widget? _) => CustomPaint(
                  painter: ViewerInkPainter(
                    ink: ink,
                    surface: widget.inkSurface,
                    sourceWidth: widget._pointerWidth.toDouble(),
                    sourceHeight: widget._pointerHeight.toDouble(),
                  ),
                ),
              ),
            ),
          ],
        ),
      );
    }
    if (widget.drawEnabled) {
      return MouseRegion(
        cursor: SystemMouseCursors.precise,
        onExit: (_) => widget.onCursorHintExit?.call(),
        child: Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (PointerDownEvent event) {
            widget.onFocusRequested?.call();
            if (_drawPointer != null ||
                (!_isDevicePointer(event) &&
                    event.buttons & kPrimaryMouseButton == 0)) {
              return;
            }
            _drawPointer = event.pointer;
            _lastDrawFrame = _eventFrame(event, box);
            widget.onDrawStart?.call(_lastDrawFrame);
          },
          onPointerMove: (PointerMoveEvent event) {
            if (_drawPointer != event.pointer) return;
            _lastDrawFrame = _eventFrame(event, box);
            widget.onDrawMove?.call(_lastDrawFrame);
            widget.onCursorHint?.call(_lastDrawFrame);
          },
          onPointerUp: (PointerUpEvent event) {
            if (_drawPointer != event.pointer) return;
            _lastDrawFrame = _eventFrame(event, box);
            widget.onDrawMove?.call(_lastDrawFrame);
            widget.onDrawEnd?.call(_lastDrawFrame);
            _drawPointer = null;
          },
          onPointerCancel: (PointerCancelEvent event) {
            if (_drawPointer != event.pointer) return;
            _drawPointer = null;
            widget.onDrawCancel?.call();
          },
          onPointerHover: (PointerHoverEvent event) {
            widget.onCursorHint?.call(_eventFrame(event, box));
          },
          onPointerSignal: (event) => _handlePointerSignal(event, box),
          child: texture,
        ),
      );
    }
    if (!widget.captureInput) {
      // The host-side pointer hint keeps working while control is off, so the
      // picture still needs hover tracking even though no button, scroll or
      // key event may reach the host in this mode.
      return MouseRegion(
        onExit: (_) {
          _stopPointerPan();
          widget.onCursorHintExit?.call();
        },
        child: Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (PointerDownEvent event) {
            _stopPointerPan();
            widget.onFocusRequested?.call();
            if (event.buttons & kPrimaryMouseButton != 0) {
              widget.onCursorHintPress?.call(_eventFrame(event, box));
            }
          },
          onPointerMove: (PointerMoveEvent event) =>
              widget.onCursorHint?.call(_eventFrame(event, box)),
          onPointerHover: (event) => _handleHover(event, box),
          onPointerSignal: (event) => _handlePointerSignal(event, box),
          child: texture,
        ),
      );
    }
    return MouseRegion(
      cursor: widget.hideCursor ? SystemMouseCursors.none : MouseCursor.defer,
      onExit: (_) {
        _stopPointerPan();
        widget.onCursorHintExit?.call();
        _releaseButtons();
      },
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: (PointerDownEvent event) {
          _stopPointerPan();
          widget.onFocusRequested?.call();
          final Offset position = _eventFrame(event, box);
          if (_isDevicePointer(event)) {
            widget.onDevicePointer?.call(event, position);
            return;
          }
          if (event.buttons & kPrimaryMouseButton != 0) {
            widget.onCursorHintPress?.call(position);
          }
          _syncButtons(event.buttons, position);
        },
        onPointerUp: (PointerUpEvent event) {
          final Offset position = _eventFrame(event, box);
          if (_isDevicePointer(event)) {
            widget.onDevicePointer?.call(event, position);
            return;
          }
          _syncButtons(event.buttons, position);
        },
        onPointerMove: (PointerMoveEvent event) {
          final Offset position = _eventFrame(event, box);
          if (_isDevicePointer(event)) {
            widget.onDevicePointer?.call(event, position);
            return;
          }
          _syncButtons(event.buttons, position);
          widget.onCursorHint?.call(position);
          widget.onMove?.call(position, event.buttons);
        },
        onPointerHover: (event) => _handleHover(event, box),
        onPointerCancel: (PointerCancelEvent event) {
          final Offset position = _eventFrame(event, box);
          if (_isDevicePointer(event)) {
            widget.onDeviceCancel?.call(event, position);
            return;
          }
          _syncButtons(0, position);
        },
        onPointerPanZoomStart: (PointerPanZoomStartEvent event) {
          _stopPointerPan();
          widget.onFocusRequested?.call();
          _releaseButtons();
          _panZoomPointer = event.pointer;
          _resetGesture();
        },
        onPointerPanZoomUpdate: (PointerPanZoomUpdateEvent event) {
          if (_panZoomPointer != event.pointer ||
              !event.scale.isFinite ||
              event.scale <= 0) {
            return;
          }
          _gesture(
            _eventFrame(event, box),
            event.localPanDelta,
            event.scale / _panZoomScale,
          );
          _panZoomScale = event.scale;
        },
        onPointerPanZoomEnd: (PointerPanZoomEndEvent event) {
          if (_panZoomPointer != event.pointer) return;
          _panZoomPointer = null;
          _resetGesture();
        },
        onPointerSignal: (PointerSignalEvent event) {
          _handlePointerSignal(event, box);
        },
        child: texture,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    if (widget.videoWidth <= 0 || widget.videoHeight <= 0) {
      return const SizedBox.shrink();
    }
    final Size videoSize = Size(
      widget.videoWidth.toDouble(),
      widget.videoHeight.toDouble(),
    );

    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        Size available = constraints.biggest;
        if (!available.isFinite) available = constraints.constrain(videoSize);
        if (available.isEmpty) return const SizedBox.shrink();
        final fitScale = widget.fit == ViewFit.adaptive
            ? math.min(
                available.width / videoSize.width,
                available.height / videoSize.height,
              )
            : 1.0;
        final scale = fitScale * widget.zoomPercent / 100.0;
        final drawn = videoSize * scale;
        final image = SizedBox(
          key: _frameKey,
          width: drawn.width,
          height: drawn.height,
          child: _layer(drawn),
        );
        if (drawn.width <= available.width + 0.01 &&
            drawn.height <= available.height + 0.01) {
          return Center(child: image);
        }
        return _scrollableView(drawn, available, image);
      },
    );
  }

  Widget _scrollableView(Size drawn, Size available, Widget image) {
    final showBars = widget.scrollStyle == ViewScrollStyle.scrollbar;
    // Keep thumb/track presses outside the input surface, even in control mode.
    final gutter = math.min(12.0, available.shortestSide / 4);
    final vertical =
        showBars &&
        (drawn.height > available.height ||
            drawn.height > available.height - gutter &&
                drawn.width > available.width);
    final horizontal =
        showBars && drawn.width > available.width - (vertical ? gutter : 0);
    final viewport = Size(
      available.width - (vertical ? gutter : 0),
      available.height - (horizontal ? gutter : 0),
    );
    final physics = widget.captureInput || widget.drawEnabled
        ? const NeverScrollableScrollPhysics()
        : const ClampingScrollPhysics();
    Widget canvas = SizedBox.fromSize(
      key: _viewportKey,
      size: viewport,
      child: SingleChildScrollView(
        controller: _vertical,
        physics: physics,
        child: SingleChildScrollView(
          controller: _horizontal,
          physics: physics,
          scrollDirection: Axis.horizontal,
          child: ConstrainedBox(
            constraints: BoxConstraints(
              minWidth: viewport.width,
              minHeight: viewport.height,
            ),
            child: Center(child: image),
          ),
        ),
      ),
    );
    canvas = Padding(
      padding: EdgeInsets.only(
        right: vertical ? gutter : 0,
        bottom: horizontal ? gutter : 0,
      ),
      child: canvas,
    );
    if (horizontal) {
      canvas = Scrollbar(
        controller: _horizontal,
        thumbVisibility: true,
        interactive: true,
        notificationPredicate: (notification) =>
            notification.metrics.axis == Axis.horizontal,
        child: canvas,
      );
    }
    if (vertical) {
      canvas = Scrollbar(
        controller: _vertical,
        thumbVisibility: true,
        interactive: true,
        notificationPredicate: (notification) =>
            notification.metrics.axis == Axis.vertical,
        child: canvas,
      );
    }
    return ScrollConfiguration(
      behavior: ScrollConfiguration.of(context).copyWith(scrollbars: false),
      child: canvas,
    );
  }
}
