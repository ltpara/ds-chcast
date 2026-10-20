// Checks the cursor helpers that have no GPU dependency: the content id, the
// outline treatment and the bounded shape cache. Runs as part of `ctest`.
//
// The decoders themselves (monochrome / color / masked color) need a
// duplication to feed real DXGI data and are exercised by chast-capture.

#include "capture/detail.h"

#include <cstdio>
#include <cstring>
#include <string>

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

    const uint64_t id_a = chast::detail::cursor_content_id(16, 16, POINT{1, 2}, a.color);
    const uint64_t id_b = chast::detail::cursor_content_id(16, 16, POINT{1, 2}, b.color);
    const uint64_t id_c = chast::detail::cursor_content_id(16, 16, POINT{1, 2}, c.color);
    const uint64_t id_hotspot = chast::detail::cursor_content_id(16, 16, POINT{3, 2}, a.color);

    EXPECT(id_a == id_b);        // identical sprites hash the same
    EXPECT(id_a != id_c);        // a pixel difference separates them
    EXPECT(id_a != id_hotspot);  // the hotspot is part of the identity
    EXPECT(id_a <= (1ull << 53) - 1ull);
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

}  // namespace

int main() {
    test_content_id();
    test_outline();
    test_cache();
    if (g_failures == 0) {
        std::printf("cursor tests passed\n");
        return 0;
    }
    std::printf("%d cursor test(s) failed\n", g_failures);
    return 1;
}
