import 'package:flutter/material.dart';

import '../display_stream_controller.dart';
import '../l10n.dart';

class DisplayStreamView extends StatelessWidget {
  const DisplayStreamView({
    super.key,
    required this.texture,
    required this.focused,
    required this.onFocus,
    required this.onClose,
    required this.view,
  });

  final DisplayTexture texture;
  final bool focused;
  final VoidCallback onFocus;
  final VoidCallback? onClose;
  final Widget view;

  @override
  Widget build(BuildContext context) => Container(
    decoration: BoxDecoration(
      border: Border.all(
        color: focused ? Colors.lightBlueAccent : Colors.white24,
        width: 2,
      ),
    ),
    child: Column(
      children: [
        Material(
          color: const Color(0xff24252b),
          child: InkWell(
            onTap: onFocus,
            child: SizedBox(
              height: 36,
              child: Row(
                children: [
                  const SizedBox(width: 10),
                  Expanded(
                    child: Text(
                      tr('显示器 {p1}', {'p1': texture.stream.displayIndex + 1}),
                      style: const TextStyle(color: Colors.white),
                    ),
                  ),
                  IconButton(
                    tooltip: tr('关闭此屏画面'),
                    padding: EdgeInsets.zero,
                    onPressed: onClose,
                    icon: const Icon(Icons.close, size: 18),
                  ),
                ],
              ),
            ),
          ),
        ),
        Expanded(
          child: texture.stream.ready
              ? view
              : Center(
                  child: Text(
                    texture.stream.error.isEmpty
                        ? tr('等待此屏画面')
                        : texture.stream.error,
                    style: const TextStyle(color: Colors.white70),
                  ),
                ),
        ),
      ],
    ),
  );
}

/// Cursor positions and sprite sizes are source pixels, independent of the
/// encoder's downscaled picture and of the neighbouring display's geometry.
Widget displayCursorOverlay(DisplayTexture texture, Size drawn) {
  final stream = texture.stream;
  final cursor = texture.pointer;
  final image = texture.cursor;
  if (cursor == null ||
      !cursor.visible ||
      image == null ||
      texture.cursorShape != cursor.shapeId) {
    return const SizedBox.shrink();
  }
  final width = stream.sourceWidth > 0 ? stream.sourceWidth : stream.width;
  final height = stream.sourceHeight > 0 ? stream.sourceHeight : stream.height;
  if (width <= 0 || height <= 0) return const SizedBox.shrink();
  final x = drawn.width / width, y = drawn.height / height;
  // The protocol reports the hotspot, so the sprite's top-left sits before it.
  return Stack(
    children: [
      Positioned(
        left: (cursor.x - cursor.hotspotX) * x,
        top: (cursor.y - cursor.hotspotY) * y,
        width: cursor.width * x,
        height: cursor.height * y,
        child: RawImage(
          image: image,
          fit: BoxFit.fill,
          filterQuality: FilterQuality.low,
        ),
      ),
    ],
  );
}
