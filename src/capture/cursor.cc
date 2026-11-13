#include "capture/detail.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace chast {
namespace {

bool read_cursor_registry_dword(HKEY key, const wchar_t* subkey, const wchar_t* value,
                                DWORD& result) {
    DWORD bytes = sizeof(result);
    return RegGetValueW(key, subkey, value, RRF_RT_REG_DWORD, nullptr, &result,
                        &bytes) == ERROR_SUCCESS &&
        bytes == sizeof(result);
}

// The capture API runs at per-monitor DPI awareness. CursorBaseSize is stored
// DPI-independently, so the physical diameter must be resolved against this
// output's monitor rather than the process or primary monitor.
}  // namespace

bool sample_desktop_cursor(const DisplayInfo& display, CursorInfo& cursor) {
    CURSORINFO info{sizeof(info)};
    if (!GetCursorInfo(&info)) return false;
    CursorInfo next;
    next.position_valid = true;
    next.visible = (info.flags & CURSOR_SHOWING) && info.hCursor &&
        info.ptScreenPos.x >= display.rect.left && info.ptScreenPos.x < display.rect.right &&
        info.ptScreenPos.y >= display.rect.top && info.ptScreenPos.y < display.rect.bottom;
    next.position = {info.ptScreenPos.x - display.rect.left, info.ptScreenPos.y - display.rect.top};
    if (info.hCursor) {
        ICONINFO icon{};
        if (!GetIconInfo(info.hCursor, &icon)) return false;
        struct Bitmaps {
            HBITMAP color, mask;
            ~Bitmaps() { if (color) DeleteObject(color); if (mask) DeleteObject(mask); }
        } bitmaps{icon.hbmColor, icon.hbmMask};
        BITMAP size{};
        if (!GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(size), &size)) return false;
        const UINT source_width = size.bmWidth;
        const UINT source_height = icon.hbmColor ? size.bmHeight : size.bmHeight / 2;
        if (!source_width || !source_height || source_width > 2048 ||
            source_height > 2048) {
            return false;
        }
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO format{};
        format.bmiHeader = {sizeof(BITMAPINFOHEADER), static_cast<LONG>(source_width),
            -static_cast<LONG>(source_height), 1, 32, BI_RGB};
        void* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &format, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dib) { DeleteDC(dc); return false; }
        const auto previous = SelectObject(dc, dib);
        const size_t bytes = static_cast<size_t>(source_width) * source_height * 4;
        std::memset(pixels, 0, bytes);
        const bool black_ok = DrawIconEx(dc, 0, 0, info.hCursor, source_width,
                                        source_height, 0, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        detail::CursorBitmap bitmap;
        bitmap.width = source_width;
        bitmap.height = source_height;
        bitmap.color.assign(static_cast<uint8_t*>(pixels), static_cast<uint8_t*>(pixels) + bytes);
        bitmap.invert.resize(bytes / 4);
        std::memset(pixels, 255, bytes);
        const bool white_ok = DrawIconEx(dc, 0, 0, info.hCursor, source_width,
                                        source_height, 0, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        const auto* white = static_cast<const uint8_t*>(pixels);
        for (size_t i = 0; i < bytes / 4; ++i) {
            auto* black = bitmap.color.data() + i * 4;
            const int difference = int(white[i * 4]) - black[0];
            if (difference < 0) {
                bitmap.invert[i] = 1;
                std::memset(black, 0, 4);
            } else black[3] = static_cast<uint8_t>(255 - difference);
        }
        SelectObject(dc, previous); DeleteObject(dib); DeleteDC(dc);
        if (!black_ok || !white_ok) return false;

        // GetIconInfo normally returns the cursor's base bitmap, not the size
        // Windows displays after the accessibility slider or monitor DPI
        // scaling. GetCursorInfo reports the hotspot in that source bitmap;
        // the shared derivation scales both and adds any outline offset.
        const POINT source_hotspot{static_cast<LONG>(icon.xHotspot),
                                   static_cast<LONG>(icon.yHotspot)};
        detail::CursorBitmap sprite;
        if (!detail::derive_cursor_sprite(bitmap, source_hotspot,
                                          detail::cursor_target_size(display), sprite,
                                          next.hotspot, next.origin_offset)) {
            return false;
        }
        bitmap = std::move(sprite);
        next.width = bitmap.width; next.height = bitmap.height; next.pitch = next.width * 4;
        next.bitmap = std::move(bitmap.color); next.invert = std::move(bitmap.invert);
        next.outlined = bitmap.outlined;
        next.shape_type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
        next.shape_valid = true;
        next.content_id = detail::cursor_content_id(
            next.width, next.height, next.hotspot, next.bitmap, next.invert);
        detail::cursor_shape_cache().put(next);
    }
    next.revision = cursor.revision + (next.position.x != cursor.position.x ||
        next.position.y != cursor.position.y || next.visible != cursor.visible ||
        next.content_id != cursor.content_id || next.position_valid != cursor.position_valid);
    cursor = std::move(next);
    return true;
}
} // namespace chast

namespace chast::detail {

UINT cursor_target_size(const DisplayInfo& display) {
    DWORD base_size = 0;
    DWORD accessibility_size = 0;
    const bool have_base = read_cursor_registry_dword(
        HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", base_size);
    const bool have_accessibility = read_cursor_registry_dword(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"CursorSize",
        accessibility_size);
    const UINT base = cursor_diameter_from_settings(
        have_base ? base_size : 0, have_accessibility ? accessibility_size : 0);
    if (base == 0) {
        return 0;
    }

    HMONITOR monitor = display.monitor;
    if (monitor == nullptr) {
        RECT rect{display.rect.left, display.rect.top, display.rect.right,
                  display.rect.bottom};
        monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    }
    UINT dpi_x = 96;
    UINT dpi_y = 96;
    if (monitor == nullptr ||
        FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) ||
        dpi_x == 0) {
        dpi_x = 96;
    }
    return cursor_size_at_dpi(base, dpi_x);
}

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

bool pointer_shape_valid(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, UINT data_size,
                         UINT max_bytes) {
    if (shape.Width == 0 || shape.Height == 0 || shape.Pitch == 0) {
        return false;
    }
    const uint64_t width = shape.Width;
    const uint64_t height = shape.Height;
    const uint64_t pitch = shape.Pitch;
    uint64_t tail_bytes = 0;
    uint64_t decoded_height = height;
    switch (shape.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
            // Every decoder reads four bytes per pixel across a full row, so a
            // short pitch would walk past the returned bytes.
            if (pitch < width * 4u) {
                return false;
            }
            if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                tail_bytes = static_cast<uint64_t>(mask_row_bytes(shape.Width)) * height;
            }
            break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
            // The AND and XOR planes are stacked, so the reported height must
            // be even and each plane row must fit the pitch.
            if ((shape.Height & 1u) != 0 || pitch < mask_row_bytes(shape.Width)) {
                return false;
            }
            decoded_height = height / 2u;
            break;
        default:
            return false;
    }
    // Compare through division: a hostile driver can report dimensions whose
    // product wraps 64 bits, which would turn a rounded-up length check into a
    // pass. decoded_height and height are non-zero here.
    if (tail_bytes > data_size || pitch > (static_cast<uint64_t>(data_size) - tail_bytes) / height) {
        return false;
    }
    return width <= static_cast<uint64_t>(max_bytes) / 4u / decoded_height;
}

UINT cursor_diameter_from_settings(UINT cursor_base_size, UINT accessibility_size) {
    UINT diameter = 0;
    if (cursor_base_size >= 32 && cursor_base_size <= 256) {
        diameter = cursor_base_size;
    }
    if (accessibility_size >= 1 && accessibility_size <= 15) {
        diameter = std::max<UINT>(diameter, 32u + (accessibility_size - 1u) * 16u);
    }
    return diameter;
}

UINT cursor_size_at_dpi(UINT base_diameter, UINT dpi) {
    if (base_diameter == 0) {
        return 0;
    }
    if (dpi == 0) {
        dpi = 96;
    }
    const uint64_t scaled = (static_cast<uint64_t>(base_diameter) * dpi + 48u) / 96u;
    return static_cast<UINT>(std::clamp<uint64_t>(
        scaled, 1u, static_cast<uint64_t>(kMaxCursorSourceSize)));
}

void fit_cursor_dimensions(UINT source_width, UINT source_height,
                           UINT target_diameter, UINT& width, UINT& height) {
    width = source_width;
    height = source_height;
    if (source_width == 0 || source_height == 0 || target_diameter == 0) {
        return;
    }
    const UINT longest = std::max<UINT>(source_width, source_height);
    const UINT target = std::min<UINT>(target_diameter, kMaxCursorSourceSize);
    const uint64_t scaled_width =
        (static_cast<uint64_t>(source_width) * target + longest / 2u) / longest;
    const uint64_t scaled_height =
        (static_cast<uint64_t>(source_height) * target + longest / 2u) / longest;
    width = static_cast<UINT>(std::clamp<uint64_t>(scaled_width, 1u, target));
    height = static_cast<UINT>(std::clamp<uint64_t>(scaled_height, 1u, target));
}

POINT scale_cursor_hotspot(POINT hotspot, UINT source_width, UINT source_height,
                           UINT width, UINT height) {
    if (source_width == 0 || source_height == 0 || width == 0 || height == 0) {
        return hotspot;
    }
    auto scale_axis = [](LONG value, UINT source, UINT target) {
        if (value <= 0) {
            return LONG{0};
        }
        const uint64_t scaled =
            (static_cast<uint64_t>(value) * target + source / 2u) / source;
        return static_cast<LONG>(std::min<uint64_t>(scaled, target - 1u));
    };
    return POINT{scale_axis(hotspot.x, source_width, width),
                 scale_axis(hotspot.y, source_height, height)};
}

bool scale_cursor_bitmap(const CursorBitmap& bitmap, UINT width, UINT height,
                         CursorBitmap& out) {
    if (width == 0 || height == 0 || bitmap.width == 0 || bitmap.height == 0 ||
        bitmap.color.size() < static_cast<size_t>(bitmap.width) * bitmap.height * 4u ||
        (!bitmap.invert.empty() &&
         bitmap.invert.size() < static_cast<size_t>(bitmap.width) * bitmap.height)) {
        return false;
    }
    if (width == bitmap.width && height == bitmap.height) {
        out = bitmap;
        return true;
    }

    out.width = width;
    out.height = height;
    out.color.resize(static_cast<size_t>(width) * height * 4u);
    out.invert.resize(static_cast<size_t>(width) * height);
    out.outlined = bitmap.outlined;
    const double scale_x = static_cast<double>(bitmap.width) / width;
    const double scale_y = static_cast<double>(bitmap.height) / height;

    for (UINT y = 0; y < height; ++y) {
        const double sy = std::clamp((y + 0.5) * scale_y - 0.5, 0.0,
                                     static_cast<double>(bitmap.height - 1u));
        const UINT y0 = static_cast<UINT>(std::floor(sy));
        const UINT y1 = std::min<UINT>(y0 + 1u, bitmap.height - 1u);
        const double fy = sy - y0;
        const UINT invert_y = std::min<UINT>(
            static_cast<UINT>((y + 0.5) * scale_y), bitmap.height - 1u);
        for (UINT x = 0; x < width; ++x) {
            const double sx = std::clamp((x + 0.5) * scale_x - 0.5, 0.0,
                                         static_cast<double>(bitmap.width - 1u));
            const UINT x0 = static_cast<UINT>(std::floor(sx));
            const UINT x1 = std::min<UINT>(x0 + 1u, bitmap.width - 1u);
            const double fx = sx - x0;
            const double weights[4] = {
                (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
                (1.0 - fx) * fy, fx * fy,
            };
            const UINT xs[4] = {x0, x1, x0, x1};
            const UINT ys[4] = {y0, y0, y1, y1};
            uint8_t* dst = out.color.data() + (static_cast<size_t>(y) * width + x) * 4u;
            for (size_t channel = 0; channel < 4; ++channel) {
                double value = 0.0;
                for (size_t sample = 0; sample < 4; ++sample) {
                    const size_t source_index =
                        (static_cast<size_t>(ys[sample]) * bitmap.width + xs[sample]) * 4u;
                    value += bitmap.color[source_index + channel] * weights[sample];
                }
                dst[channel] = static_cast<uint8_t>(std::clamp(
                    std::lround(value), 0l, 255l));
            }
            const UINT invert_x = std::min<UINT>(
                static_cast<UINT>((x + 0.5) * scale_x), bitmap.width - 1u);
            out.invert[static_cast<size_t>(y) * width + x] =
                bitmap.invert.empty()
                    ? 0
                    : bitmap.invert[static_cast<size_t>(invert_y) * bitmap.width + invert_x];
        }
    }
    return true;
}

bool derive_cursor_sprite(const CursorBitmap& decoded, POINT source_hotspot,
                          UINT target_diameter, CursorBitmap& sprite,
                          POINT& hotspot, POINT& origin_offset) {
    if (decoded.width == 0 || decoded.height == 0 ||
        decoded.color.size() < static_cast<size_t>(decoded.width) * decoded.height * 4u) {
        return false;
    }

    CursorBitmap scaled;
    UINT width = decoded.width;
    UINT height = decoded.height;
    fit_cursor_dimensions(decoded.width, decoded.height, target_diameter, width, height);
    if ((width != decoded.width || height != decoded.height) &&
        !scale_cursor_bitmap(decoded, width, height, scaled)) {
        return false;
    }
    const CursorBitmap& source = scaled.color.empty() ? decoded : scaled;

    CursorBitmap outlined;
    if (outline_cursor(source, source.width, source.height, outlined)) {
        sprite = std::move(outlined);
        origin_offset = POINT{1, 1};
    } else {
        sprite = source;
        origin_offset = POINT{0, 0};
    }
    const POINT scaled_hotspot = scale_cursor_hotspot(
        source_hotspot, decoded.width, decoded.height, source.width, source.height);
    hotspot = POINT{scaled_hotspot.x + origin_offset.x,
                    scaled_hotspot.y + origin_offset.y};
    return true;
}

uint64_t cursor_content_id(UINT width, UINT height, POINT hotspot,
                           const std::vector<uint8_t>& color,
                           const std::vector<uint8_t>& invert) {
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
    // The same colour bytes with a different inversion mask are not the same
    // sprite: consumers composite the masked pixels with DST_INVERT.
    hash = mix(hash, invert.size());
    const size_t invert_blocks = invert.size() / sizeof(uint64_t);
    for (size_t i = 0; i < invert_blocks; ++i) {
        uint64_t word = 0;
        std::memcpy(&word, invert.data() + i * sizeof(uint64_t), sizeof(word));
        hash = mix(hash, word);
    }
    uint64_t invert_tail = 0;
    if (!invert.empty()) {
        std::memcpy(&invert_tail, invert.data() + invert_blocks * sizeof(uint64_t),
                    invert.size() % sizeof(uint64_t));
    }
    hash = mix(hash, invert_tail);
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

void CursorDecoder::set_target_size(UINT diameter) {
    if (target_size_ == diameter) {
        return;
    }
    target_size_ = diameter;
    if (decoded_.width != 0 && decoded_.height != 0) {
        derive_sprite();
        revision_++;
    }
}

void CursorDecoder::derive_sprite() {
    if (decoded_.width == 0 || decoded_.height == 0) {
        return;
    }

    // DXGI's HotSpot is relative to the unscaled shape it returned. Scale it
    // with the sprite, then add the outline offset introduced above.
    const POINT source_hotspot{static_cast<LONG>(shape_.HotSpot.x),
                               static_cast<LONG>(shape_.HotSpot.y)};
    if (!derive_cursor_sprite(decoded_, source_hotspot, target_size_, bitmap_, hotspot_,
                              origin_offset_)) {
        clear_shape();
        return;
    }
    content_id_ = cursor_content_id(bitmap_.width, bitmap_.height, hotspot_, bitmap_.color,
                                    bitmap_.invert);
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
    if (data_size > raw_.size() || !pointer_shape_valid(actual, data_size, max_bitmap_bytes_)) {
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

    // Keep the unscaled DXGI decode. Accessibility size and DPI can change
    // while the driver reports that the shape itself is unchanged; deriving
    // the published sprite from this cache lets that update still take effect.
    shape_ = actual;
    decoded_ = std::move(decoded);
    derive_sprite();
    revision_++;
}

void CursorDecoder::clear_shape() {
    const bool had_shape = has_shape();
    shape_ = {};
    decoded_ = {};
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.contains(info.content_id)) {
        return;
    }
    const size_t entry_bytes = info.bitmap.size() + info.invert.size();
    entries_.emplace(info.content_id, std::make_shared<const CursorInfo>(info));
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
        bytes_ -= it->second->bitmap.size() + it->second->invert.size();
        entries_.erase(it);
        return;
    }
}

std::shared_ptr<const CursorInfo> CursorShapeCache::find(uint64_t content_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(content_id);
    return it == entries_.end() ? nullptr : it->second;
}

size_t CursorShapeCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t CursorShapeCache::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

void CursorShapeCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    order_.clear();
    bytes_ = 0;
}

CursorShapeCache& cursor_shape_cache() {
    static CursorShapeCache cache;
    return cache;
}

}  // namespace chast::detail
