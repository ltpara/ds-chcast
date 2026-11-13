// Checks the cursor helpers that have no GPU dependency: identity, the
// accessibility/DPI size mapping, premultiplied scaling, outline treatment and
// the bounded shape cache. Runs as part of `ctest`.
//
// The decoders themselves (monochrome / color / masked color) need a
// duplication to feed real DXGI data and are exercised by chast-capture.

#include "capture/detail.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

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

chast::detail::CursorBitmap solid(UINT width, UINT height, uint8_t b, uint8_t g, uint8_t r,
                                  uint8_t a) {
    chast::detail::CursorBitmap bitmap;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.color.assign(static_cast<size_t>(width) * height * 4u, 0);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        uint8_t* px = bitmap.color.data() + i * 4u;
        px[0] = b;
        px[1] = g;
        px[2] = r;
        px[3] = a;
    }
    bitmap.invert.assign(static_cast<size_t>(width) * height, 0);
    return bitmap;
}

void test_content_id() {
    const auto a = solid(16, 16, 1, 2, 3, 255);
    const auto b = solid(16, 16, 1, 2, 3, 255);
    const auto c = solid(16, 16, 1, 2, 4, 255);

    const uint64_t id_a = chast::detail::cursor_content_id(
        16, 16, POINT{1, 2}, a.color, a.invert);
    const uint64_t id_b = chast::detail::cursor_content_id(
        16, 16, POINT{1, 2}, b.color, b.invert);
    const uint64_t id_c = chast::detail::cursor_content_id(
        16, 16, POINT{1, 2}, c.color, c.invert);
    const uint64_t id_hotspot = chast::detail::cursor_content_id(
        16, 16, POINT{3, 2}, a.color, a.invert);
    auto inverted = a.invert;
    inverted[7] = 1;
    const uint64_t id_invert = chast::detail::cursor_content_id(
        16, 16, POINT{1, 2}, a.color, inverted);

    EXPECT(id_a == id_b);        // identical sprites hash the same
    EXPECT(id_a != id_c);        // a pixel difference separates them
    EXPECT(id_a != id_hotspot);  // the hotspot is part of the identity
    EXPECT(id_a != id_invert);   // compositing semantics are part of identity
    EXPECT(id_a <= (1ull << 53) - 1ull);
}

void test_accessibility_size() {
    using chast::detail::cursor_diameter_from_settings;
    using chast::detail::cursor_size_at_dpi;
    using chast::detail::fit_cursor_dimensions;

    EXPECT(cursor_diameter_from_settings(32, 0) == 32);
    EXPECT(cursor_diameter_from_settings(0, 1) == 32);
    EXPECT(cursor_diameter_from_settings(0, 13) == 224);
    EXPECT(cursor_diameter_from_settings(0, 15) == 256);
    // A partially applied Accessibility write must not leave a large pointer
    // at its stale 32 px base size.
    EXPECT(cursor_diameter_from_settings(32, 15) == 256);
    EXPECT(cursor_diameter_from_settings(64, 1) == 64);
    // Missing or corrupt registry values leave the reported sprite alone.
    EXPECT(cursor_diameter_from_settings(0, 0) == 0);
    EXPECT(cursor_diameter_from_settings(31, 16) == 0);

    EXPECT(cursor_size_at_dpi(32, 96) == 32);
    EXPECT(cursor_size_at_dpi(32, 144) == 48);
    EXPECT(cursor_size_at_dpi(224, 96) == 224);
    EXPECT(cursor_size_at_dpi(256, 192) == 510);
    EXPECT(cursor_size_at_dpi(256, 0) == 256);

    UINT width = 0;
    UINT height = 0;
    fit_cursor_dimensions(32, 32, 224, width, height);
    EXPECT(width == 224 && height == 224);
    fit_cursor_dimensions(32, 64, 224, width, height);
    EXPECT(width == 112 && height == 224);
    fit_cursor_dimensions(1024, 512, 256, width, height);
    EXPECT(width == 256 && height == 128);
    fit_cursor_dimensions(32, 32, 1000, width, height);
    EXPECT(width == chast::detail::kMaxCursorSourceSize);
    EXPECT(height == chast::detail::kMaxCursorSourceSize);
    fit_cursor_dimensions(32, 16, 0, width, height);
    EXPECT(width == 32 && height == 16);
}

void test_scaled_hotspot() {
    POINT point = chast::detail::scale_cursor_hotspot(POINT{3, 7}, 16, 16, 64, 64);
    EXPECT(point.x == 12 && point.y == 28);
    point = chast::detail::scale_cursor_hotspot(POINT{16, 16}, 16, 16, 64, 64);
    EXPECT(point.x == 63 && point.y == 63);
    point = chast::detail::scale_cursor_hotspot(POINT{-1, -2}, 16, 16, 64, 64);
    EXPECT(point.x == 0 && point.y == 0);
}

void test_scaled_bitmap() {
    chast::detail::CursorBitmap in = solid(2, 2, 100, 50, 25, 128);
    in.invert[0] = 1;
    in.invert[3] = 1;
    chast::detail::CursorBitmap out;
    EXPECT(chast::detail::scale_cursor_bitmap(in, 4, 4, out));
    EXPECT(out.width == 4 && out.height == 4);
    EXPECT(out.color.size() == 4u * 4u * 4u);
    EXPECT(out.invert.size() == 4u * 4u);

    // Premultiplied channels are interpolated directly. An all-opaque source
    // stays exact, including the scaled inversion mask.
    const auto* centre = out.color.data() + (2u * 4u + 2u) * 4u;
    EXPECT(centre[0] == 100 && centre[1] == 50 && centre[2] == 25 && centre[3] == 128);
    EXPECT(out.invert[0] == 1 && out.invert[10] == 1);
    EXPECT(out.invert[3] == 0);

    // Interpolating a transparent, premultiplied black pixel with an opaque red
    // pixel must fade colour and alpha together: no dark fringe and no channel
    // above alpha.
    chast::detail::CursorBitmap edge = solid(2, 1, 0, 0, 0, 0);
    edge.color[4] = 0;
    edge.color[5] = 0;
    edge.color[6] = 255;
    edge.color[7] = 255;
    chast::detail::CursorBitmap stretched;
    EXPECT(chast::detail::scale_cursor_bitmap(edge, 4, 1, stretched));
    for (UINT x = 0; x < 4; ++x) {
        const uint8_t* px = stretched.color.data() + x * 4u;
        EXPECT(px[0] <= px[3] && px[1] <= px[3] && px[2] <= px[3]);
    }
    EXPECT(stretched.color[3] == 0);
    EXPECT(stretched.color[15] == 255);

    // Invalid geometry never partially overwrites the destination.
    chast::detail::CursorBitmap bad;
    bad.width = bad.height = 2;
    bad.color.resize(4);
    chast::detail::CursorBitmap untouched;
    EXPECT(!chast::detail::scale_cursor_bitmap(bad, 4, 4, untouched));
    EXPECT(untouched.width == 0 && untouched.color.empty());
}

void test_derive_sprite() {
    chast::detail::CursorBitmap decoded = solid(16, 16, 80, 40, 20, 255);
    decoded.invert[5] = 1;
    chast::detail::CursorBitmap sprite;
    POINT hotspot{};
    POINT origin{};

    EXPECT(chast::detail::derive_cursor_sprite(decoded, POINT{4, 8}, 32, sprite,
                                               hotspot, origin));
    EXPECT(sprite.width == 34 && sprite.height == 34);
    EXPECT(sprite.outlined && sprite.invert.empty());
    EXPECT(origin.x == 1 && origin.y == 1);
    EXPECT(hotspot.x == 9 && hotspot.y == 17);

    // Re-deriving the same unscaled decode at a different accessibility size
    // produces the new sprite even though the source bytes did not change.
    EXPECT(chast::detail::derive_cursor_sprite(decoded, POINT{4, 8}, 80, sprite,
                                               hotspot, origin));
    EXPECT(sprite.width == 82 && sprite.height == 82);
    EXPECT(hotspot.x == 21 && hotspot.y == 41);

    // No configured size leaves the decoded dimensions alone, but still maps
    // the hotspot and applies the outline consistently.
    EXPECT(chast::detail::derive_cursor_sprite(decoded, POINT{4, 8}, 0, sprite,
                                               hotspot, origin));
    EXPECT(sprite.width == 18 && sprite.height == 18);
    EXPECT(hotspot.x == 5 && hotspot.y == 9);
}

void test_outline() {
    // A 3x3 opaque sprite whose centre inverts the desktop. Outlining makes it
    // black and wraps the sprite in a white ring, so the result is 5x5 with
    // the shape at (1,1).
    chast::detail::CursorBitmap in = solid(3, 3, 200, 100, 50, 255);
    in.invert[4] = 1;  // centre pixel: y == 1, x == 1

    chast::detail::CursorBitmap out;
    EXPECT(chast::detail::outline_cursor(in, 3, 3, out));
    EXPECT(out.outlined);
    EXPECT(out.width == 5 && out.height == 5);
    EXPECT(out.color.size() == 5u * 5u * 4u);
    EXPECT(out.invert.empty());

    auto at = [&](UINT x, UINT y) { return out.color.data() + (static_cast<size_t>(y) * 5u + x) * 4u; };
    // The inverted pixel became opaque black.
    EXPECT(at(2, 2)[0] == 0 && at(2, 2)[1] == 0 && at(2, 2)[2] == 0 && at(2, 2)[3] == 255);
    // The ring is opaque white where it borders the sprite.
    EXPECT(at(2, 0)[0] == 255 && at(2, 0)[3] == 255);
    EXPECT(at(0, 2)[0] == 255 && at(0, 2)[3] == 255);
    EXPECT(at(4, 2)[0] == 255 && at(4, 2)[3] == 255);
    EXPECT(at(2, 4)[0] == 255 && at(2, 4)[3] == 255);
    // The corners only touch other ring pixels, so they stay transparent.
    EXPECT(at(0, 0)[3] == 0 && at(4, 4)[3] == 0);
    // A non-inverted sprite pixel keeps its premultiplied colour and is not
    // overwritten by the ring.
    EXPECT(at(1, 1)[0] == 200 && at(1, 1)[1] == 100 && at(1, 1)[2] == 50 && at(1, 1)[3] == 255);

    // A sprite without inversion is not outlined.
    const auto plain = solid(8, 8, 10, 20, 30, 255);
    chast::detail::CursorBitmap untouched;
    EXPECT(!chast::detail::outline_cursor(plain, 8, 8, untouched));
    EXPECT(!untouched.outlined);
}

DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info(UINT type, UINT width, UINT height, UINT pitch) {
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
    info.Type = type;
    info.Width = width;
    info.Height = height;
    info.Pitch = pitch;
    return info;
}

void test_pointer_shape_valid() {
    using chast::detail::pointer_shape_valid;
    constexpr UINT kColor = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    constexpr UINT kMasked = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    constexpr UINT kMono = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;

    // A 16x16 colour pointer is 64 bytes per row; the reported size must match.
    EXPECT(pointer_shape_valid(shape_info(kColor, 16, 16, 64), 1024, 4096));
    EXPECT(!pointer_shape_valid(shape_info(kColor, 16, 16, 64), 1023, 4096));
    // A pitch below width * 4 would make the decoder read past each row.
    EXPECT(!pointer_shape_valid(shape_info(kColor, 16, 16, 60), 4096, 4096));
    // The decoded sprite is bounded separately from the source bytes.
    EXPECT(!pointer_shape_valid(shape_info(kColor, 16, 16, 64), 1024, 1023));
    // Degenerate geometry never decodes.
    EXPECT(!pointer_shape_valid(shape_info(kColor, 0, 16, 64), 1024, 4096));
    EXPECT(!pointer_shape_valid(shape_info(kColor, 16, 0, 64), 1024, 4096));
    EXPECT(!pointer_shape_valid(shape_info(kColor, 16, 16, 0), 1024, 4096));
    // Unknown shape types are rejected rather than decoded as colour.
    EXPECT(!pointer_shape_valid(shape_info(7, 16, 16, 64), 1024, 4096));

    // Masked colour adds a 1bpp mask plane per row after the pixels: width 16
    // rounds to a 4-byte mask row, so 16x2 needs 64*2 + 4*2 == 136 bytes.
    EXPECT(pointer_shape_valid(shape_info(kMasked, 16, 2, 64), 136, 4096));
    EXPECT(!pointer_shape_valid(shape_info(kMasked, 16, 2, 64), 135, 4096));

    // Monochrome stacks AND and XOR planes: height is doubled, each plane row
    // spans the 4-byte mask row, and the decoded height is halved.
    EXPECT(pointer_shape_valid(shape_info(kMono, 16, 4, 4), 16, 4096));
    EXPECT(!pointer_shape_valid(shape_info(kMono, 16, 3, 4), 16, 4096));
    EXPECT(!pointer_shape_valid(shape_info(kMono, 16, 4, 2), 16, 4096));
    EXPECT(!pointer_shape_valid(shape_info(kMono, 16, 4, 4), 15, 4096));
    // 16x4 source decodes to 16x2, i.e. 128 bytes of RGBA.
    EXPECT(!pointer_shape_valid(shape_info(kMono, 16, 4, 4), 16, 127));
}

chast::CursorInfo cursor_info(uint64_t id, size_t bytes) {
    chast::CursorInfo info;
    info.shape_valid = true;
    info.content_id = id;
    info.width = static_cast<UINT>(bytes / 4u);
    info.height = 1;
    info.bitmap.assign(bytes, 0);
    return info;
}

void test_cache() {
    chast::detail::CursorShapeCache cache;
    cache.put(cursor_info(1, 16));
    cache.put(cursor_info(2, 32));
    EXPECT(cache.size() == 2);
    EXPECT(cache.find(1) != nullptr);
    EXPECT(cache.find(3) == nullptr);

    // Re-putting a cached id is a no-op, not a duplicate.
    const size_t before = cache.bytes();
    cache.put(cursor_info(1, 16));
    EXPECT(cache.size() == 2 && cache.bytes() == before);

    // Shapes without an id are not cached.
    cache.put(cursor_info(0, 16));
    chast::CursorInfo invalid = cursor_info(9, 16);
    invalid.shape_valid = false;
    cache.put(invalid);
    EXPECT(cache.size() == 2);

    // Entry bound: the oldest entries go first.
    for (size_t i = 0; i < chast::detail::CursorShapeCache::kMaxEntries + 16; ++i) {
        cache.put(cursor_info(1000 + i, 4));
    }
    EXPECT(cache.size() <= chast::detail::CursorShapeCache::kMaxEntries);
    EXPECT(cache.find(1) == nullptr);

    // Byte bound.
    cache.clear();
    const size_t chunk = 4u * 1024u * 1024u;
    for (size_t i = 0; i < 10; ++i) {
        cache.put(cursor_info(100 + i, chunk));
    }
    EXPECT(cache.bytes() <= chast::detail::CursorShapeCache::kMaxBytes);
    EXPECT(cache.find(109) != nullptr);
}

void print_cursor(const char* path, const chast::CursorInfo& cursor) {
    std::printf("%s valid=%d visible=%d size=%ux%u hotspot=%ld,%ld offset=%ld,%ld id=%llu\n",
                path, cursor.shape_valid ? 1 : 0, cursor.visible ? 1 : 0,
                cursor.width, cursor.height, cursor.hotspot.x, cursor.hotspot.y,
                cursor.origin_offset.x, cursor.origin_offset.y,
                static_cast<unsigned long long>(cursor.content_id));
}

bool real_cursor_size_valid(UINT target, const chast::CursorInfo& cursor) {
    if (!cursor.shape_valid || !cursor.visible || cursor.width == 0 || cursor.height == 0) {
        return false;
    }
    const UINT span = (std::max)(cursor.width, cursor.height);
    if (span > chast::detail::kMaxCursorSpriteSize) {
        return false;
    }
    if (target == 0) {
        // No usable per-user setting: keeping the source dimensions is valid.
        return true;
    }
    const UINT scaled = (std::min)(target, chast::detail::kMaxCursorSourceSize);
    // The accessibility outline can add one pixel on each side.
    return span >= scaled && span <= scaled + 2;
}

int run_real_cursor_probe(bool check_dxgi) {
    const auto displays = chast::enumerate_displays();
    if (displays.empty()) {
        std::printf("real cursor probe: no displays\n");
        return 1;
    }
    const auto found = std::find_if(displays.begin(), displays.end(),
                                    [](const auto& display) { return display.primary; });
    const chast::DisplayInfo& display = found == displays.end() ? displays.front() : *found;
    const UINT target = chast::detail::cursor_target_size(display);
    std::printf("target=%u\n", target);

    chast::CursorInfo desktop;
    if (!chast::sample_desktop_cursor(display, desktop)) {
        std::printf("desktop: sample failed\n");
        return 1;
    }
    print_cursor("desktop", desktop);
    if (!real_cursor_size_valid(target, desktop)) {
        std::printf("desktop: shape invalid, hidden, or does not match target=%u\n", target);
        return 1;
    }

    if (!check_dxgi) {
        return 0;
    }

    chast::CaptureConfig config;
    config.backend = chast::CaptureBackend::Dxgi;
    config.capture_cursor = true;
    std::string error;
    auto capturer = chast::create_capturer(display, config, &error);
    if (!capturer) {
        std::printf("dxgi: create failed: %s\n", error.c_str());
        return 1;
    }
    chast::CursorInfo dxgi;
    POINT original_pointer{};
    const bool restore_pointer = GetCursorPos(&original_pointer) != FALSE;
    const int nudge_x = display.rect.left + (display.rect.right - display.rect.left) / 2;
    const int nudge_y = display.rect.top + (display.rect.bottom - display.rect.top) / 2;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if ((attempt % 5) == 0) {
            // A first duplication may start after the pointer's last hardware
            // update. Move it by one pixel within the output so DXGI publishes
            // the shape; the caller-visible position is restored afterwards.
            SetCursorPos(nudge_x + (attempt / 5) % 2, nudge_y);
            mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
        }
        const auto result = capturer->capture(std::chrono::milliseconds(100));
        std::printf("dxgi attempt=%d result=%d shape=%d error=%s\n", attempt,
                    static_cast<int>(result), capturer->cursor().shape_valid ? 1 : 0,
                    capturer->last_error().message().c_str());
        if (result == chast::CaptureResult::FatalError) {
            std::printf("dxgi: capture fatal: %s\n", capturer->last_error().message().c_str());
            return 1;
        }
        if (capturer->cursor().shape_valid) {
            dxgi = capturer->cursor();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    if (restore_pointer) {
        SetCursorPos(original_pointer.x, original_pointer.y);
    }
    if (!dxgi.shape_valid) {
        std::printf("dxgi: no pointer shape after retries\n");
        return 1;
    }
    print_cursor("dxgi", dxgi);
    if (!real_cursor_size_valid(target, dxgi)) {
        std::printf("dxgi: shape does not match target=%u\n", target);
        return 1;
    }
    return 0;
}

}  // namespace


int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--real") == 0) {
        return run_real_cursor_probe(true);
    }
    if (argc == 2 && std::strcmp(argv[1], "--real-gdi") == 0) {
        return run_real_cursor_probe(false);
    }
    test_content_id();
    test_accessibility_size();
    test_scaled_hotspot();
    test_scaled_bitmap();
    test_derive_sprite();
    test_outline();
    test_pointer_shape_valid();
    test_cache();
    if (g_failures == 0) {
        std::printf("cursor tests passed\n");
        return 0;
    }
    std::printf("%d cursor test(s) failed\n", g_failures);
    return 1;
}
