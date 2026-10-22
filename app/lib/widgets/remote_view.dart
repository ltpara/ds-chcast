import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../viewer_bridge.dart';

enum ViewFit { adaptive, original }

/// Pointer callbacks report positions in captured-frame pixels: (0, 0) is the
/// frame's top-left, before the virtual-desktop origin is added. The protocol
/// wants virtual-desktop coordinates, so the session page does that last step.
typedef FramePointerHandler = void Function(Offset position, int buttons);
typedef FrameButtonHandler = void Function(Offset position, int button, bool down);
typedef FrameScrollHandler = void Function(Offset position, Offset delta);

const int _mouseButtonMask = ViewerMouse.left |
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

class RemoteView extends StatefulWidget {
  const RemoteView({
    super.key,
    required this.textureId,
    required this.videoWidth,
    required this.videoHeight,
    required this.fit,
    this.captureInput = false,
    this.hideCursor = false,
    this.onScaleChanged,
    this.onHover,
    this.onMove,
    this.onButton,
    this.onScroll,
    this.onFocusRequested,
  });

  final int textureId;
  final int videoWidth;
  final int videoHeight;
  final ViewFit fit;

  /// When true the view forwards raw pointer events. The session page only
  /// enables this while remote control is on.
  final bool captureInput;

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
  final VoidCallback? onFocusRequested;

  @override
  State<RemoteView> createState() => _RemoteViewState();
}

class _RemoteViewState extends State<RemoteView> {
  final ScrollController _vertical = ScrollController();
  final ScrollController _horizontal = ScrollController();

  /// Buttons currently down on the local side. Pointer events carry the whole
  /// state, while the protocol wants one button per message, so down/up are
  /// derived from the difference against this.
  int _buttons = 0;
  Offset _lastFrame = Offset.zero;
  double? _reportedScale;

  @override
  void dispose() {
    _releaseButtons();
    _vertical.dispose();
    _horizontal.dispose();
    super.dispose();
  }

  /// Tells the page how large the frame is drawn, once per change. Deferred to
  /// after the frame because the page answers with a native call, and
  /// [build] must not have a side effect on the engine.
  void _reportScale(Size drawn) {
    if (widget.videoWidth <= 0 || drawn.width <= 0) {
      return;
    }
    final double scale = drawn.width / widget.videoWidth;
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
    if (oldWidget.captureInput && !widget.captureInput) {
      _releaseButtons();
    }
  }

  Offset _toFrame(Offset local, Size box) {
    if (box.width <= 0 || box.height <= 0) {
      return Offset.zero;
    }
    final double x =
        (local.dx * widget.videoWidth / box.width).clamp(0.0, widget.videoWidth - 1.0).toDouble();
    final double y =
        (local.dy * widget.videoHeight / box.height).clamp(0.0, widget.videoHeight - 1.0).toDouble();
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

  Widget _layer(Size box) {
    _reportScale(box);
    final Widget texture = Texture(
      textureId: widget.textureId,
      filterQuality: widget.fit == ViewFit.original ? FilterQuality.none : FilterQuality.low,
    );
    if (!widget.captureInput) {
      return texture;
    }
    return MouseRegion(
      cursor: widget.hideCursor ? SystemMouseCursors.none : MouseCursor.defer,
      onExit: (_) => _releaseButtons(),
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: (PointerDownEvent event) {
          widget.onFocusRequested?.call();
          _syncButtons(event.buttons, _toFrame(event.localPosition, box));
        },
        onPointerUp: (PointerUpEvent event) =>
            _syncButtons(event.buttons, _toFrame(event.localPosition, box)),
        onPointerMove: (PointerMoveEvent event) {
          final Offset position = _toFrame(event.localPosition, box);
          _syncButtons(event.buttons, position);
          widget.onMove?.call(position, event.buttons);
        },
        onPointerHover: (PointerHoverEvent event) {
          final Offset position = _toFrame(event.localPosition, box);
          _syncButtons(event.buttons, position);
          widget.onHover?.call(position, event.buttons);
        },
        onPointerCancel: (PointerCancelEvent event) =>
            _syncButtons(0, _toFrame(event.localPosition, box)),
        onPointerSignal: (PointerSignalEvent event) {
          if (event is PointerScrollEvent) {
            widget.onScroll?.call(_toFrame(event.localPosition, box), event.scrollDelta);
          }
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
    final Size videoSize =
        Size(widget.videoWidth.toDouble(), widget.videoHeight.toDouble());

    if (widget.fit == ViewFit.adaptive) {
      return LayoutBuilder(
        builder: (BuildContext context, BoxConstraints constraints) {
          Size available = constraints.biggest;
          if (!available.isFinite || available.width <= 0 || available.height <= 0) {
            available = videoSize;
          }
          final double scale = math.min(
            available.width / videoSize.width,
            available.height / videoSize.height,
          );
          final Size fitted = Size(
            videoSize.width * scale,
            videoSize.height * scale,
          );
          return Center(
            child: SizedBox(
              width: fitted.width,
              height: fitted.height,
              child: _layer(fitted),
            ),
          );
        },
      );
    }

    if (widget.captureInput) {
      // Remote control owns the wheel, so the canvas must not also scroll
      // itself. A window smaller than the frame clips instead.
      return ClipRect(
        child: OverflowBox(
          minWidth: videoSize.width,
          maxWidth: videoSize.width,
          minHeight: videoSize.height,
          maxHeight: videoSize.height,
          child: _layer(videoSize),
        ),
      );
    }

    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        return Scrollbar(
          controller: _vertical,
          child: SingleChildScrollView(
            controller: _vertical,
            child: SingleChildScrollView(
              controller: _horizontal,
              scrollDirection: Axis.horizontal,
              child: ConstrainedBox(
                constraints: BoxConstraints(
                  minWidth: constraints.maxWidth,
                  minHeight: constraints.maxHeight,
                ),
                child: Center(
                  child: SizedBox(
                    width: videoSize.width,
                    height: videoSize.height,
                    child: _layer(videoSize),
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}
