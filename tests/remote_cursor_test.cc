// Checks the viewer's sprite installer: validation, the scaled-handle cache
// and release. Building a handle only needs a screen DC, so this runs headless
// -- and this session has no visible hardware pointer, which is exactly why
// the DXGI path cannot be exercised here.

#include "remote_cursor.h"

#include <cstdio>
#include <cstring>
#include <string>
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

std::vector<uint8_t> sprite(int width, int height) {
    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4u, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* px = bgra.data() + (static_cast<size_t>(y) * width + x) * 4u;
            px[0] = 40;
            px[1] = 60;
            px[2] = 200;
            px[3] = (x == 0) ? 0 : 255;  // first column transparent
        }
    }
    return bgra;
}

void test_quantise() {
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(1.0) == 1.0);
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(1.13) == 1.25);
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(1.10) == 1.0);
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(1.9) == 2.0);
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(2.6) == 3.0);
    // Out-of-range and nonsense factors fall back to 1x, never zero.
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(0.0) == 1.0);
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(-2.0) == 1.0);
    EXPECT(chast_viewer::RemoteCursor::QuantiseScale(40.0) == 4.0);
}

void test_register_validation() {
    chast_viewer::RemoteCursor cursor;
    std::string error;
    const std::vector<uint8_t> pixels = sprite(8, 8);
    EXPECT(!cursor.Register(0, 8, 8, 0, 0, pixels, &error));  // no id, no shape
    EXPECT(cursor.Register(1, 8, 8, 0, 0, pixels, &error));
    // A sprite that does not match its declared size is refused.
    EXPECT(!cursor.Register(2, 9, 8, 0, 0, pixels, &error));
    EXPECT(error.find("does not match") != std::string::npos);
    // Bigger than the OS cursor cap is refused up front.
    EXPECT(!cursor.Register(3, 300, 4, 0, 0, sprite(300, 4), &error));
    // A repeated id is a no-op, not an error.
    EXPECT(cursor.Register(1, 8, 8, 0, 0, pixels, &error));
    EXPECT(!cursor.active());
    EXPECT(cursor.cursor() == nullptr);
}

void test_activate_and_cache() {
    chast_viewer::RemoteCursor cursor;
    std::string error;
    EXPECT(cursor.Register(7, 32, 32, 3, 5, sprite(32, 32), &error));
    EXPECT(cursor.Register(9, 16, 8, 0, 0, sprite(16, 8), &error));

    EXPECT(!cursor.Activate(8, 1.0, &error));  // never registered
    EXPECT(error == "the cursor shape was never registered");

    EXPECT(cursor.Activate(7, 1.0, &error));
    HCURSOR one = cursor.cursor();
    EXPECT(one != nullptr);
    EXPECT(cursor.active());

    // Same shape, same scale: the cached handle, not a rebuild.
    EXPECT(cursor.Activate(7, 1.0, &error));
    EXPECT(cursor.cursor() == one);

    // A different shape swaps the handle.
    EXPECT(cursor.Activate(9, 1.0, &error));
    EXPECT(cursor.cursor() != nullptr);
    EXPECT(cursor.cursor() != one);

    // A fractional scale builds a second handle for the first shape.
    EXPECT(cursor.Activate(7, 1.5, &error));
    HCURSOR half = cursor.cursor();
    EXPECT(half != nullptr);
    EXPECT(half != one);
    EXPECT(cursor.Activate(7, 1.0, &error));
    EXPECT(cursor.cursor() == one);

    cursor.Deactivate();
    EXPECT(!cursor.active());
    EXPECT(cursor.cursor() == nullptr);
}

void test_oversized_scale_shrinks() {
    chast_viewer::RemoteCursor cursor;
    std::string error;
    EXPECT(cursor.Register(11, 200, 100, 0, 0, sprite(200, 100), &error));
    // 4x would be 800 px wide, far past the icon cap; it shrinks instead of
    // failing, so the pointer stays visible.
    EXPECT(cursor.Activate(11, 4.0, &error));
    EXPECT(cursor.active());
}

void test_clear_releases_and_resets() {
    chast_viewer::RemoteCursor cursor;
    std::string error;
    EXPECT(cursor.Register(5, 16, 16, 1, 1, sprite(16, 16), &error));
    EXPECT(cursor.Activate(5, 1.0, &error));
    cursor.Clear();
    EXPECT(!cursor.active());
    EXPECT(cursor.cursor() == nullptr);
    // The sprite is gone too, so a stale id cannot be activated.
    EXPECT(!cursor.Activate(5, 1.0, &error));
}

void test_shape_table_is_bounded() {
    chast_viewer::RemoteCursor cursor;
    std::string error;
    const std::vector<uint8_t> pixels = sprite(4, 4);
    // A host that keeps inventing ids must not grow the viewer's table
    // without bound; the oldest sprite is the one dropped.
    for (uint64_t id = 1; id <= 80; ++id) {
        EXPECT(cursor.Register(id, 4, 4, 0, 0, pixels, &error));
    }
    EXPECT(!cursor.Activate(1, 1.0, &error));   // evicted
    EXPECT(cursor.Activate(80, 1.0, &error));   // still held
}

void test_handle_matches_shape() {
    chast_viewer::RemoteCursor cursor;
    std::string error;
    EXPECT(cursor.Register(21, 32, 32, 3, 5, sprite(32, 32), &error));
    EXPECT(cursor.Activate(21, 2.0, &error));
    HCURSOR handle = cursor.cursor();
    EXPECT(handle != nullptr);
    if (handle == nullptr) {
        return;
    }
    // The OS must see a real cursor of the scaled size with the hotspot moved
    // with it; that is what makes the pointer land where the user clicks.
    ICONINFO info{};
    EXPECT(GetIconInfo(handle, &info) != FALSE);
    BITMAP color{};
    EXPECT(info.hbmColor != nullptr && GetObject(info.hbmColor, sizeof(color), &color) > 0);
    EXPECT(color.bmWidth == 64);
    EXPECT(color.bmHeight == 64);
    EXPECT(info.xHotspot == 6 && info.yHotspot == 10);
    if (info.hbmColor) {
        DeleteObject(info.hbmColor);
    }
    if (info.hbmMask) {
        DeleteObject(info.hbmMask);
    }
}

}  // namespace

int main() {
    test_quantise();
    test_register_validation();
    test_activate_and_cache();
    test_oversized_scale_shrinks();
    test_clear_releases_and_resets();
    test_shape_table_is_bounded();
    test_handle_matches_shape();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d cursor installer checks failed\n", g_failures);
        return 1;
    }
    std::printf("remote cursor checks passed\n");
    return 0;
}
