#pragma once

// Pure raster helpers for the temporary session annotation overlay. The
// Win32 window and monitor planes live in annotation_overlay.cc; keeping the
// pixel maths here lets a test exercise clipping, antialiasing and
// premultiplied alpha without an interactive desktop.

#include <cstdint>

namespace chast {

// Draws one round-capped segment into a tightly packed premultiplied BGRA
// canvas. Coordinates are local to the canvas. A segment outside the canvas
// is clipped by the bounds loop; a zero-length segment draws a dot.
void annotation_draw_segment(uint32_t* pixels, int width, int height, int x0, int y0,
                             int x1, int y1, double radius, uint32_t color,
                             uint8_t alpha);

}  // namespace chast
