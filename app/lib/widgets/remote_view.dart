import 'package:flutter/material.dart';

enum ViewFit { adaptive, original }

class RemoteView extends StatefulWidget {
  const RemoteView({
    super.key,
    required this.textureId,
    required this.videoWidth,
    required this.videoHeight,
    required this.fit,
  });

  final int textureId;
  final int videoWidth;
  final int videoHeight;
  final ViewFit fit;

  @override
  State<RemoteView> createState() => _RemoteViewState();
}

class _RemoteViewState extends State<RemoteView> {
  final ScrollController _vertical = ScrollController();
  final ScrollController _horizontal = ScrollController();

  @override
  void dispose() {
    _vertical.dispose();
    _horizontal.dispose();
    super.dispose();
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

    if (widget.fit == ViewFit.adaptive) {
      return Center(
        child: AspectRatio(
          aspectRatio: aspect,
          child: texture,
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
                    child: texture,
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
