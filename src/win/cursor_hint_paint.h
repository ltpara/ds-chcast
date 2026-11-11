#pragma once

// Pixel maths for the "show my cursor" hint. The window, the DIB and the
// layered-window calls stay in show_cursor.cc; everything here is pure, so a
// test can check the raster without a desktop.

#include <cstdint>
#include <string>

namespace chast {

// Surface border kept free around the arrow so the click ripple can expand
// without resizing the window. Scales with the cursor size.
int cursor_hint_margin(int arrow_width);

// Ripple geometry for a click, timed by `progress` in [0, 1]. The ring starts
// just outside the arrow tip and fades as it expands.
double cursor_hint_ripple_radius(int arrow_width, double progress);
int cursor_hint_ripple_alpha(double progress);

// A stable, saturated colour for a viewer's device name, so two viewers are
// told apart. An empty name keeps the plain white arrow.
uint32_t cursor_hint_color(const std::string& label);

struct CursorHintLayout {
    int width = 0;
    int height = 0;
    int arrow_x = 0;
    int arrow_y = 0;
    // Arrow tip inside the surface: the surface is placed so this point lands
    // on the viewer's pointer.
    int tip_x = 0;
    int tip_y = 0;
    // Label origin inside the surface and its clipped extent.
    int label_x = 0;
    int label_y = 0;
    int label_width = 0;
    int label_height = 0;
};

CursorHintLayout cursor_hint_layout(int arrow_width, int arrow_height, int hotspot_x,
                                    int hotspot_y, int label_width, int label_height);

struct CursorHintSurface {
    int width = 0;
    int height = 0;
    int arrow_x = 0;
    int arrow_y = 0;
    int arrow_width = 0;
    int arrow_height = 0;
    int tip_x = 0;
    int tip_y = 0;
    // 0xRRGGBB fill for the arrow, the ripple and the label.
    uint32_t color = 0xFFFFFF;
    // < 0 draws no ripple; otherwise the animation progress in [0, 1].
    double ripple = -1.0;
    // 8-bit glyph coverage, drawn in `color` with a black offset shadow.
    const uint8_t* label_mask = nullptr;
    int label_width = 0;
    int label_height = 0;
    int label_x = 0;
    int label_y = 0;
};

// Fills the whole surface with premultiplied BGRA; pixels outside the arrow,
// the ripple and the label become fully transparent.
void paint_cursor_hint(uint32_t* pixels, const CursorHintSurface& surface);

}  // namespace chast
