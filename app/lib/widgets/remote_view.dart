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

class RemoteView extends StatefulWidget {
  const RemoteView({
    super.key,
    required this.textureId,
    required this.videoWidth,
    required this.videoHeight,
    required this.fit,
    this.captureInput = false,
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

  /// When true the view hides the local pointer and forwards raw pointer
  /// events. The session page only enables this while remote control is on.
  final bool captureInput;
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

  @override
  void dispose() {
    _vertical.dispose();
    _horizontal.dispose();
    super.dispose();
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

  void _pointerButton(PointerEvent event, bool down, Size box) {
    final int changed = down ? event.buttons & ~_buttons : _buttons & ~event.buttons;
    _buttons = event.buttons;
    widget.onFocusRequested?.call();
    if (changed == 0) {
      return;
    }
    final Offset position = _toFrame(event.localPosition, box);
    for (final int bit in <int>[
      ViewerMouse.left,
      ViewerMouse.right,
      ViewerMouse.middle,
      ViewerMouse.back,
      ViewerMouse.forward,
    ]) {
      if (changed & bit != 0) {
        widget.onButton?.call(position, bit, down);
      }
    }
  }

  Widget _layer(Size box) {
    final Widget texture = Texture(
      textureId: widget.textureId,
      filterQuality: widget.fit == ViewFit.original ? FilterQuality.none : FilterQuality.low,
    );
    if (!widget.captureInput) {
      return texture;
    }
    return MouseRegion(
      cursor: SystemMouseCursors.none,
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: (PointerDownEvent event) => _pointerButton(event, true, box),
        onPointerUp: (PointerUpEvent event) => _pointerButton(event, false, box),
        onPointerMove: (PointerMoveEvent event) {
          _buttons = event.buttons;
          widget.onMove?.call(_toFrame(event.localPosition, box), event.buttons);
        },
        onPointerHover: (PointerHoverEvent event) {
          _buttons = event.buttons;
          widget.onHover?.call(_toFrame(event.localPosition, box), event.buttons);
        },
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
