// Pure checks for the "show my cursor" raster: the click ripple, the viewer
// colour and label, and the premultiplied-alpha invariant the layered window
// relies on. No desktop or window is created.

#include "win/cursor_hint_paint.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(cond)                                                             \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__,     \
                         __LINE__, #cond);                                       \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

chast::CursorHintSurface layout_surface(int arrow_width, int arrow_height, int hotspot_x,
                                        int hotspot_y, uint32_t color,
                                        const std::vector<uint8_t>& label_mask,
                                        int label_width, int label_height) {
    const chast::CursorHintLayout layout = chast::cursor_hint_layout(
        arrow_width, arrow_height, hotspot_x, hotspot_y, label_width, label_height);
    chast::CursorHintSurface surface;
    surface.width = layout.width;
    surface.height = layout.height;
    surface.arrow_x = layout.arrow_x;
    surface.arrow_y = layout.arrow_y;
    surface.arrow_width = arrow_width;
    surface.arrow_height = arrow_height;
    surface.tip_x = layout.tip_x;
    surface.tip_y = layout.tip_y;
    surface.color = color;
    surface.label_mask = label_mask.empty() ? nullptr : label_mask.data();
    surface.label_width = label_width;
    surface.label_height = label_height;
    surface.label_x = layout.label_x;
    surface.label_y = layout.label_y;
    return surface;
}

uint32_t alpha_at(const std::vector<uint32_t>& pixels, int width, int x, int y) {
    return (pixels[static_cast<size_t>(y) * width + x] >> 24) & 0xFFu;
}

void expect_premultiplied(const std::vector<uint32_t>& pixels) {
    bool ok = true;
    for (const uint32_t pixel : pixels) {
        const uint32_t alpha = (pixel >> 24) & 0xFFu;
        const uint32_t red = (pixel >> 16) & 0xFFu;
        const uint32_t green = (pixel >> 8) & 0xFFu;
        const uint32_t blue = pixel & 0xFFu;
        if (red > alpha || green > alpha || blue > alpha) {
            ok = false;
            break;
        }
    }
    EXPECT(ok);
}

void test_color() {
    EXPECT(chast::cursor_hint_color("") == 0xFFFFFF);
    EXPECT(chast::cursor_hint_color("DESKTOP-ALPHA") == chast::cursor_hint_color("DESKTOP-ALPHA"));
    // A name picks one of the palette entries, never white.
    EXPECT(chast::cursor_hint_color("DESKTOP-ALPHA") != 0xFFFFFF);
    int distinct = 0;
    const char* names[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
    uint32_t seen[6] = {};
    for (const char* name : names) {
        const uint32_t color = chast::cursor_hint_color(name);
        bool fresh = true;
        for (int i = 0; i < distinct; ++i) {
            if (seen[i] == color) {
                fresh = false;
            }
        }
        if (fresh) {
            seen[distinct++] = color;
        }
    }
    EXPECT(distinct >= 3);
}

void test_layout() {
    EXPECT(chast::cursor_hint_margin(32) == 20);
    EXPECT(chast::cursor_hint_margin(48) == 29);
    const chast::CursorHintLayout bare =
        chast::cursor_hint_layout(32, 32, 2, 1, 0, 0);
    EXPECT(bare.width == 32 + 2 * chast::cursor_hint_margin(32));
    EXPECT(bare.height == 32 + 2 * chast::cursor_hint_margin(32));
    EXPECT(bare.tip_x == chast::cursor_hint_margin(32) + 2);
    EXPECT(bare.tip_y == chast::cursor_hint_margin(32) + 1);

    const chast::CursorHintLayout labelled =
        chast::cursor_hint_layout(32, 32, 2, 1, 120, 16);
    EXPECT(labelled.width > bare.width);
    EXPECT(labelled.label_x > labelled.tip_x);
    EXPECT(labelled.label_y > labelled.tip_y);
    EXPECT(labelled.label_x + labelled.label_width <= labelled.width);
    EXPECT(labelled.label_y + labelled.label_height <= labelled.height);
}

void test_arrow() {
    // Pure blue tells the fill apart from the black stroke without relying on
    // GDI's exact antialiasing.
    const chast::CursorHintSurface surface =
        layout_surface(32, 32, 2, 1, 0x0000FF, {}, 0, 0);
    std::vector<uint32_t> pixels(static_cast<size_t>(surface.width) * surface.height,
                                 0xDEADBEEFu);
    chast::paint_cursor_hint(pixels.data(), surface);

    EXPECT(alpha_at(pixels, surface.width, 0, 0) == 0);
    EXPECT(alpha_at(pixels, surface.width, surface.width - 1, surface.height - 1) == 0);
    expect_premultiplied(pixels);

    int opaque = 0;
    bool fill = false;
    for (const uint32_t pixel : pixels) {
        const uint32_t alpha = (pixel >> 24) & 0xFFu;
        if (alpha > 128) {
            ++opaque;
        }
        if (alpha >= 200 && (pixel & 0xFFu) > 100) {
            fill = true;
        }
        EXPECT(((pixel >> 16) & 0xFFu) == 0);  // blue fill, black stroke, no red
    }
    EXPECT(opaque > 100);
    EXPECT(opaque < surface.width * surface.height / 2);
    EXPECT(fill);
}

void test_ripple() {
    EXPECT(chast::cursor_hint_ripple_radius(32, 0.0) == 4.0);
    EXPECT(chast::cursor_hint_ripple_radius(32, 1.0) == 18.0);
    EXPECT(chast::cursor_hint_ripple_radius(64, 1.0) == 36.0);
    EXPECT(chast::cursor_hint_ripple_alpha(0.0) > chast::cursor_hint_ripple_alpha(0.5));
    EXPECT(chast::cursor_hint_ripple_alpha(0.5) > chast::cursor_hint_ripple_alpha(1.0));
    EXPECT(chast::cursor_hint_ripple_alpha(1.0) == 0);

    chast::CursorHintSurface surface =
        layout_surface(32, 32, 2, 1, 0x00FF00, {}, 0, 0);
    surface.ripple = 0.0;
    std::vector<uint32_t> pixels(static_cast<size_t>(surface.width) * surface.height, 0);
    chast::paint_cursor_hint(pixels.data(), surface);
    expect_premultiplied(pixels);
    const int radius = static_cast<int>(chast::cursor_hint_ripple_radius(32, 0.0));
    // The ring sits left of the arrow, where nothing else paints.
    EXPECT(alpha_at(pixels, surface.width, surface.tip_x - radius, surface.tip_y) > 0);
    EXPECT(alpha_at(pixels, surface.width, surface.tip_x - 1, surface.tip_y - 1) == 0);
    EXPECT(alpha_at(pixels, surface.width, surface.tip_x - radius - 4,
                    surface.tip_y + radius + 4) == 0);

    // A finished ripple leaves no ring behind.
    surface.ripple = 1.0;
    std::fill(pixels.begin(), pixels.end(), 0);
    chast::paint_cursor_hint(pixels.data(), surface);
    EXPECT(alpha_at(pixels, surface.width, surface.tip_x - radius, surface.tip_y) == 0);

    // No ripple at all paints the same transparent border.
    surface.ripple = -1.0;
    std::fill(pixels.begin(), pixels.end(), 0);
    chast::paint_cursor_hint(pixels.data(), surface);
    EXPECT(alpha_at(pixels, surface.width, surface.tip_x - radius, surface.tip_y) == 0);
}

void test_label() {
    std::vector<uint8_t> mask(4, 0);
    mask[0] = 255;  // one glyph pixel at the label origin
    const chast::CursorHintSurface surface =
        layout_surface(32, 32, 2, 1, 0xFF0000, mask, 2, 2);
    std::vector<uint32_t> pixels(static_cast<size_t>(surface.width) * surface.height, 0);
    chast::paint_cursor_hint(pixels.data(), surface);
    expect_premultiplied(pixels);

    const uint32_t glyph = pixels[static_cast<size_t>(surface.label_y) * surface.width +
                                  surface.label_x];
    EXPECT(((glyph >> 24) & 0xFFu) >= 200);
    EXPECT(((glyph >> 16) & 0xFFu) > 150);  // tinted red
    const uint32_t shadow =
        pixels[static_cast<size_t>(surface.label_y + 1) * surface.width + surface.label_x + 1];
    EXPECT(((shadow >> 24) & 0xFFu) > 100);
    EXPECT(((shadow >> 24) & 0xFFu) < ((glyph >> 24) & 0xFFu));
    EXPECT((shadow & 0xFFFFFFu) == 0);
    EXPECT(alpha_at(pixels, surface.width, surface.label_x + 3, surface.label_y + 3) == 0);
}

}  // namespace

int main() {
    test_color();
    test_layout();
    test_arrow();
    test_ripple();
    test_label();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d cursor hint expectation(s) failed\n", g_failures);
        return 1;
    }
    std::printf("cursor hint paint tests passed\n");
    return 0;
}
