#include "capture/detail.h"

#include <algorithm>
#include <cstring>

namespace chast::detail {

// Pointer decoding. DXGI reports pointer shapes in three formats whose alpha
// handling differs, so each is normalized here into one of two forms the
// consumer can composite without branching on the shape type:
//
//   * MONOCHROME: AND mask followed by XOR mask, 1bpp, each row DWORD aligned.
//     (desktop AND mask) XOR xor mask. AND=1 keeps the desktop, AND=0/XOR=0 is
//     black, AND=0/XOR=1 is white -- fully expressible as an alpha sprite.
//     AND=1/XOR=1 must invert the desktop, which cannot be expressed as a
//     sprite and is exported as an invert pixel like masked color.
//   * COLOR: BGRA8 with per-pixel alpha, not premultiplied.
//   * MASKED_COLOR: BGRA8 where alpha is not coverage but an operation: 0
//     inverts the desktop pixel, nonzero replaces it. Inversion cannot be
//     expressed as a sprite, so invert pixels are exported as a separate
//     1-byte-per-pixel mask and the consumer blends them with DST_INVERT.
//
// A sprite with inverted pixels is additionally outlined (see outline_cursor).
// The ring keeps the pointer visible wherever the viewer composites it, since
// the desktop it inverts against is the viewer's own rendering and need not
// match pixel for pixel. RustDesk applies the same treatment to inverted
// cursors, borrowed from TigerVNC and tightvnc.
//
// The desktop texture is never mapped here. Pointer shapes are the only pixels
// in the capture path that reach system memory.

namespace {

size_t mask_row_bytes(UINT width) { return ((width + 31u) / 32u) * 4u; }

inline void put_premultiplied(uint8_t* out, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    out[0] = static_cast<uint8_t>(b * a / 255u);
    out[1] = static_cast<uint8_t>(g * a / 255u);
    out[2] = static_cast<uint8_t>(r * a / 255u);
    out[3] = a;
}

// 1bpp plane access with the top bit of the first byte as the leftmost pixel.
inline bool bit_at(const uint8_t* row, UINT x) { return (row[x >> 3u] & (0x80u >> (x & 7u))) != 0; }

void decode_monochrome(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, const uint8_t* src,
                       CursorBitmap& out) {
    // For monochrome pointers DXGI reports Height as twice the real height:
    // the top half of the data is the AND mask, the bottom half the XOR mask,
    // both `Height / 2` rows of Pitch bytes.
    const UINT height = shape.Height / 2u;
    const size_t pixels = static_cast<size_t>(shape.Width) * height;
    out.width = shape.Width;
    out.height = height;
    out.color.assign(pixels * 4u, 0);
    out.invert.assign(pixels, 0);
    out.outlined = false;

    const uint8_t* xor_plane = src + static_cast<size_t>(shape.Pitch) * height;

    for (UINT y = 0; y < height; ++y) {
        const uint8_t* and_row = src + static_cast<size_t>(y) * shape.Pitch;
        const uint8_t* xor_row = xor_plane + static_cast<size_t>(y) * shape.Pitch;
        for (UINT x = 0; x < shape.Width; ++x) {
            const size_t index = static_cast<size_t>(y) * shape.Width + x;
            uint8_t* px = out.color.data() + index * 4u;
            const bool keep = bit_at(and_row, x);
            const bool value = bit_at(xor_row, x);
            if (keep) {
                if (value) {
                    // AND=1, XOR=1 inverts the desktop (the I-beam over a
                    // background it wants to contrast with). The inversion has
                    // to be carried as an operation, so the pixel is left for
                    // the consumer to invert.
                    out.invert[index] = 1;
                }
                // AND=1, XOR=0 keeps the desktop: a transparent sprite pixel.
            } else if (value) {
                put_premultiplied(px, 255, 255, 255, 255);
            } else {
                put_premultiplied(px, 0, 0, 0, 255);
            }
        }
    }
}

void decode_color(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, const uint8_t* src,
                  CursorBitmap& out) {
    const size_t pixels = static_cast<size_t>(shape.Width) * shape.Height;
    out.width = shape.Width;
    out.height = shape.Height;
    out.color.resize(pixels * 4u);
    out.invert.assign(pixels, 0);
    out.outlined = false;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* in = src + i * 4u;
        put_premultiplied(out.color.data() + i * 4u, in[0], in[1], in[2], in[3]);
    }
}

void decode_masked_color(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, const uint8_t* src,
                         CursorBitmap& out) {
    const size_t pixels = static_cast<size_t>(shape.Width) * shape.Height;
    out.width = shape.Width;
    out.height = shape.Height;
    out.color.assign(pixels * 4u, 0);
    out.invert.assign(pixels, 0);
    out.outlined = false;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* in = src + i * 4u;
        if (in[3] == 0) {
            out.invert[i] = 1;  // invert operation, not a sprite pixel
        } else {
            put_premultiplied(out.color.data() + i * 4u, in[0], in[1], in[2], 255);
        }
    }
}

// xxHash-style 64-bit mix (splitmix finalizer) over a sprite's identity.
uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

}  // namespace

size_t cursor_buffer_size(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape) {
    const size_t pitch_rows = static_cast<size_t>(shape.Pitch) * shape.Height;
    if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        return pitch_rows;
    }
    if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Pitch already spans the DWORD-aligned mask row and Height is the
        // doubled AND+XOR height, so this covers both planes.
        return pitch_rows;
    }
    // Masked-color shapes carry a 1bpp mask plane trailing the pixels.
    return pitch_rows + mask_row_bytes(shape.Width) * shape.Height;
}

uint64_t cursor_content_id(UINT width, UINT height, POINT hotspot,
                           const std::vector<uint8_t>& color) {
    uint64_t hash = 0xCBF29CE484222325ull;  // FNV-1a offset basis
    hash = mix(hash, width);
    hash = mix(hash, height);
    // The hotspot is part of the identity, as in RustDesk's cursor_content_id:
    // the same pixels with a different hot spot are a different pointer as far
    // as a viewer is concerned.
    hash = mix(hash, static_cast<uint64_t>(static_cast<uint32_t>(hotspot.x)) |
                         (static_cast<uint64_t>(static_cast<uint32_t>(hotspot.y)) << 32));
    // Stream the pixels in 8-byte blocks. The hash is captured at the same
    // granularity as RustDesk's (xxh3 over the same fields) only in spirit:
    // the id is opaque on the wire, so what matters is that it is stable,
    // well distributed and identical for identical sprites.
    const size_t blocks = color.size() / sizeof(uint64_t);
    const uint8_t* bytes = color.data();
    for (size_t i = 0; i < blocks; ++i) {
        uint64_t word = 0;
        std::memcpy(&word, bytes + i * sizeof(uint64_t), sizeof(word));
        hash = mix(hash, word);
    }
    uint64_t tail = 0;
    std::memcpy(&tail, bytes + blocks * sizeof(uint64_t), color.size() % sizeof(uint64_t));
    hash = mix(hash, tail);
    hash = mix(hash, color.size());
    // 53 bits: survives a JavaScript number, which RustDesk's web client needs.
    return hash & ((1ull << 53) - 1);
}

bool outline_cursor(const CursorBitmap& bitmap, UINT width, UINT height, CursorBitmap& out) {
    if (width == 0 || height == 0 ||
        bitmap.color.size() < static_cast<size_t>(width) * height * 4u) {
        return false;
    }
    const bool has_invert =
        std::any_of(bitmap.invert.begin(), bitmap.invert.end(), [](uint8_t v) { return v != 0; });
    if (!has_invert) {
        return false;
    }

    // Once the sprite is outlined, the inversion pixels are dropped: an
    // inversion is only meaningful on the desktop the pointer is over, and a
    // remote viewer composites the sprite over its own rendering of that
    // desktop. Making them black and surrounding the sprite with white keeps
    // the pointer visible on either background, which is what the RFB
    // cursor path does and why RustDesk's cursor sprites all carry an alpha
    // channel instead of an inversion mask.
    const UINT out_width = width + 2;
    const UINT out_height = height + 2;
    out.width = out_width;
    out.height = out_height;
    out.color.assign(static_cast<size_t>(out_width) * out_height * 4u, 0);
    out.invert.clear();
    out.outlined = true;

    auto visible = [&](UINT x, UINT y) {
        if (x >= width || y >= height) {
            return false;
        }
        const size_t index = static_cast<size_t>(y) * width + x;
        return bitmap.invert[index] != 0 || bitmap.color[index * 4u + 3u] != 0;
    };
    auto put = [&](UINT x, UINT y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
        uint8_t* px = out.color.data() + (static_cast<size_t>(y) * out_width + x) * 4u;
        put_premultiplied(px, b, g, r, a);
    };

    // Pass 1: a one-pixel white ring around every visible pixel. Ring pixels
    // already written stay white, so overlapping neighbours do not erase each
    // other the way a plain per-pixel write would.
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            if (!visible(x, y)) {
                continue;
            }
            const UINT ox = x + 1;
            const UINT oy = y + 1;
            put(ox, oy - 1, 255, 255, 255, 255);
            put(ox - 1, oy, 255, 255, 255, 255);
            put(ox + 1, oy, 255, 255, 255, 255);
            put(ox, oy + 1, 255, 255, 255, 255);
        }
    }

    // Pass 2: the sprite itself, centred on the ring. Inverted pixels become
    // opaque black; everything else keeps its premultiplied colour and alpha.
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const uint8_t* src = bitmap.color.data() + index * 4u;
            if (bitmap.invert[index]) {
                put(x + 1, y + 1, 0, 0, 0, 255);
            } else if (src[3] != 0) {
                put(x + 1, y + 1, src[0], src[1], src[2], src[3]);
            }
        }
    }
    return true;
}

void CursorDecoder::update_position(const POINT& position, bool visible) {
    if (position_.x != position.x || position_.y != position.y || visible_ != visible) {
        revision_++;
    }
    position_ = position;
    position_valid_ = true;
    visible_ = visible;
}

void CursorDecoder::update_shape(IDXGIOutputDuplication* duplication) {
    if (!duplication) {
        return;
    }

    // Query into a generously sized buffer: the shape is not known until the
    // call fills it in, and DXGI sizes monochrome shapes (AND and XOR planes)
    // at twice the pixel-plane estimate. If the pointer really is bigger than
    // the cap, the next call is made with the size the first one reports.
    const UINT budget = static_cast<UINT>(
        std::min<size_t>(max_bitmap_bytes_, 4u * 1024u * 1024u));
    raw_.resize(static_cast<size_t>(budget) + 64u);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO actual{};
    UINT data_size = 0;
    HRESULT hr = duplication->GetFramePointerShape(static_cast<UINT>(raw_.size()), raw_.data(),
                                                   &data_size, &actual);
    if (hr == DXGI_ERROR_MORE_DATA) {
        // The buffer was too small; read the required size back from `actual`
        // and try once more.
        const size_t required = cursor_buffer_size(actual);
        if (required == 0 || required > max_bitmap_bytes_) {
            clear_shape();
            return;
        }
        raw_.resize(required);
        hr = duplication->GetFramePointerShape(static_cast<UINT>(raw_.size()), raw_.data(),
                                               &data_size, &actual);
    }
    if (FAILED(hr)) {
        // The pointer is hidden or owned by another process (e.g. a UAC
        // prompt); the last shape stays valid for when it comes back.
        return;
    }
    if (data_size == 0) {
        // S_OK with no bytes written means "shape unchanged": keep the
        // cached bitmap rather than clearing it.
        return;
    }
    if (cursor_buffer_size(actual) > raw_.size()) {
        clear_shape();
        return;
    }
    if (actual.Width == 0 || actual.Height == 0 ||
        !has_pointer_shape(actual)) {
        clear_shape();
        return;
    }

    CursorBitmap decoded;
    switch (actual.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
            decode_monochrome(actual, raw_.data(), decoded);
            // Report the height a consumer draws with, not the doubled storage
            // height, so the bitmap and the reported size agree.
            actual.Height /= 2u;
            break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
            decode_color(actual, raw_.data(), decoded);
            break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
            decode_masked_color(actual, raw_.data(), decoded);
            break;
        default:
            clear_shape();
            return;
    }

    // DXGI places the shape's top-left at the pointer position; outlining
    // wrapped it in a ring, so the sprite's own origin is one pixel up-left.
    CursorBitmap outlined;
    const bool applied = outline_cursor(decoded, actual.Width, actual.Height, outlined);
    if (applied) {
        bitmap_ = std::move(outlined);
        origin_offset_ = POINT{1, 1};
    } else {
        bitmap_ = std::move(decoded);
        origin_offset_ = POINT{0, 0};
    }
    // DXGI's HotSpot is relative to the shape it returned. The sprite carries
    // an extra ring pixel after outlining, so the hotspot moves with it.
    hotspot_ = POINT{static_cast<LONG>(actual.HotSpot.x) + origin_offset_.x,
                     static_cast<LONG>(actual.HotSpot.y) + origin_offset_.y};
    content_id_ = cursor_content_id(bitmap_.width, bitmap_.height, hotspot_, bitmap_.color);
    shape_ = actual;
    revision_++;
}

void CursorDecoder::clear_shape() {
    const bool had_shape = has_shape();
    shape_ = {};
    bitmap_.width = 0;
    bitmap_.height = 0;
    bitmap_.color.clear();
    bitmap_.invert.clear();
    bitmap_.outlined = false;
    origin_offset_ = POINT{0, 0};
    hotspot_ = POINT{0, 0};
    content_id_ = 0;
    if (had_shape) {
        revision_++;
    }
}

void CursorDecoder::reset() {
    clear_shape();
    position_ = {};
    position_valid_ = false;
    visible_ = false;
}

// CursorShapeCache: a bounded map of sprites by content id. RustDesk keeps the
// same map so a controller that asks for a shape again (cursor_id in its
// request) does not make the host decode it a second time.

void CursorShapeCache::put(const CursorInfo& info) {
    if (!info.shape_valid || info.content_id == 0) {
        return;
    }
    if (entries_.contains(info.content_id)) {
        return;
    }
    const size_t entry_bytes = info.bitmap.size() + info.invert.size();
    entries_.emplace(info.content_id, info);
    order_.push_back(info.content_id);
    bytes_ += entry_bytes;

    // Oldest-first eviction keeps both bounds, where RustDesk clears its map as
    // a whole when either is crossed. Evicting one at a time matters here
    // because animated pointers (a spinner, a text caret) cycle through a
    // handful of shapes and would otherwise force a re-send of all of them.
    while (entries_.size() > kMaxEntries || bytes_ > kMaxBytes) {
        evict_oldest();
    }
}

void CursorShapeCache::evict_oldest() {
    while (!order_.empty()) {
        const uint64_t id = order_.front();
        order_.pop_front();
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            continue;
        }
        bytes_ -= it->second.bitmap.size() + it->second.invert.size();
        entries_.erase(it);
        return;
    }
}

const CursorInfo* CursorShapeCache::find(uint64_t content_id) const {
    auto it = entries_.find(content_id);
    return it == entries_.end() ? nullptr : &it->second;
}

void CursorShapeCache::clear() {
    entries_.clear();
    order_.clear();
    bytes_ = 0;
}

CursorShapeCache& cursor_shape_cache() {
    static CursorShapeCache cache;
    return cache;
}

}  // namespace chast::detail
