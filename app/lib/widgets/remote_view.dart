import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

enum ViewFit { adaptive, original }

/// Pointer position inside the video picture, in remote frame pixels.
typedef VideoPointerCallback = void Function(Offset pixel);

class RemoteView extends StatefulWidget {
  const RemoteView({
    super.key,
    required this.textureId,
    required this.videoWidth,
    required this.videoHeight,
    required this.fit,
    this.interactive = false,
    this.onPointerMove,
    this.onPointerButton,
    this.onPointerWheel,
  });

  final int textureId;
  final int videoWidth;
  final int videoHeight;
  final ViewFit fit;

  /// When true, pointer events that land on the picture are reported in
  /// remote frame pixels instead of being swallowed. Coordinates are clamped
  /// to the picture, so a drag that leaves the window still produces the
  /// nearest in-picture point rather than a jump.
  final bool interactive;
  final VideoPointerCallback? onPointerMove;
  final void Function(int buttonBit, bool down, Offset pixel)? onPointerButton;
  final void Function(int deltaX, int deltaY, Offset pixel)? onPointerWheel;

  @override
  State<RemoteView> createState() => _RemoteViewState();
}

class _RemoteViewState extends State<RemoteView> {
  final ScrollController _vertical = ScrollController();
  final ScrollController _horizontal = ScrollController();

  // Buttons this view believes are down, so a pointer-up outside the picture
  // (or a lost window) can still release everything it pressed.
  int _held = 0;

  @override
  void dispose() {
    _releaseButtons(null);
    _vertical.dispose();
    _horizontal.dispose();
    super.dispose();
  }

  @override
  void didUpdateWidget(covariant RemoteView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.interactive && !widget.interactive) {
      _releaseButtons(null);
    }
  }

  void _releaseButtons(Offset? pixel) {
    if (_held == 0) {
      return;
    }
    final int held = _held;
    _held = 0;
    for (int bit = 1; bit <= 16; bit <<= 1) {
      if (held & bit != 0) {
        widget.onPointerButton?.call(bit, false, pixel ?? Offset.zero);
      }
    }
  }

  /// Reconciles the buttons this view believes are down with the bitmask the
  /// embedder reports. It has to be done this way because the Windows embedder
  /// clears the released bit before sending the up event, and reports a second
  /// button pressed while another is held as a move rather than a down, so
  /// neither down nor up alone can be trusted to name the button.
  void _syncButtons(int buttons, Offset pixel) {
    final int target = buttons & 0x1F;
    final int changed = _held ^ target;
    if (changed == 0) {
      return;
    }
    _held = target;
    for (int bit = 1; bit <= 16; bit <<= 1) {
      if (changed & bit != 0) {
        widget.onPointerButton?.call(bit, target & bit != 0, pixel);
      }
    }
  }

  Offset _toVideoPixel(Offset local, Size box) {
    final double x = local.dx / box.width * widget.videoWidth;
    final double y = local.dy / box.height * widget.videoHeight;
    return Offset(
      x.clamp(0, widget.videoWidth - 1).toDouble(),
      y.clamp(0, widget.videoHeight - 1).toDouble(),
    );
  }

  Widget _wrapInteractive(Widget child) {
    if (!widget.interactive) {
      return child;
    }
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final Size box = constraints.biggest;
        if (box.isEmpty) {
          return child;
        }
        Offset pixel(Offset local) => _toVideoPixel(local, box);
        return MouseRegion(
          cursor: SystemMouseCursors.none,
          onExit: (_) => _releaseButtons(null),
          child: Listener(
            behavior: HitTestBehavior.opaque,
            onPointerDown: (PointerDownEvent event) {
              _syncButtons(event.buttons, pixel(event.localPosition));
            },
            onPointerUp: (PointerUpEvent event) {
              _syncButtons(event.buttons, pixel(event.localPosition));
            },
            onPointerMove: (PointerMoveEvent event) {
              final Offset at = pixel(event.localPosition);
              _syncButtons(event.buttons, at);
              widget.onPointerMove?.call(at);
            },
            onPointerHover: (PointerHoverEvent event) {
              _syncButtons(event.buttons, pixel(event.localPosition));
              widget.onPointerMove?.call(pixel(event.localPosition));
            },
            onPointerCancel: (PointerCancelEvent event) {
              _syncButtons(0, pixel(event.localPosition));
            },
            onPointerSignal: (PointerSignalEvent event) {
              if (event is! PointerScrollEvent) {
                return;
              }
              widget.onPointerWheel?.call(
                event.scrollDelta.dx.sign.toInt() * 120,
                event.scrollDelta.dy.sign.toInt() * 120,
                pixel(event.localPosition),
              );
            },
            child: child,
          ),
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    if (widget.videoWidth <= 0 || widget.videoHeight <= 0) {
      return const SizedBox.shrink();
    }
    final double aspect = widget.videoWidth / widget.videoHeight;
    final Widget texture = Texture(
      textureId: widget.textureId,
      filterQuality: widget.fit == ViewFit.original ? FilterQuality.none : FilterQuality.low,
    );
    final Widget video = _wrapInteractive(texture);

    if (widget.fit == ViewFit.adaptive) {
      return Center(
        child: AspectRatio(
          aspectRatio: aspect,
          child: video,
        ),
      );
    }

    final Size videoSize =
        Size(widget.videoWidth.toDouble(), widget.videoHeight.toDouble());
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
                    child: video,
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
