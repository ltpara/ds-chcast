#include "remote_cursor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace chast_viewer {

namespace {

// An icon is capped at 256x256 by the OS; asking for more fails and would
// leave the viewer with no pointer at all.
constexpr int kMaxCursorSide = 256;
constexpr size_t kMaxShapeBytes = 4u * 1024u * 1024u;

// A real desktop cycles through a few dozen pointers at most (arrow, I-beam,
// hand, the eight resize arrows, the wait spinner), so this only bounds what a
// misbehaving host could make the viewer hold. QuantiseScale can produce at
// most nine distinct factors, so the per-shape handle map never has to evict.
constexpr size_t kMaxShapes = 64;
constexpr size_t kMaxScalesPerShape = 12;

HBITMAP build_color_bitmap(HDC dc, int width, int height, const std::vector<uint8_t>& bgra) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    // Negative height: the rows are top-down, which is the order DXGI hands
    // shapes over in.
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }
    std::memcpy(pixels, bgra.data(), static_cast<size_t>(width) * height * 4u);
    return bitmap;
}

// The AND mask: 1 keeps the screen underneath, 0 lets the colour bitmap show.
// It is built from alpha, so a colour pixel that happens to be black (an
// outlined pointer's ring shadow) is not mistaken for a transparent one.
HBITMAP build_mask_bitmap(int width, int height, const std::vector<uint8_t>& bgra) {
    const int stride = ((width + 31) / 32) * 4;
    std::vector<uint8_t> bits(static_cast<size_t>(stride) * height, 0);
    for (int y = 0; y < height; ++y) {
        // Bitmaps are bottom-up: source row y lands in destination row height-1-y.
        uint8_t* row = bits.data() + static_cast<size_t>(height - 1 - y) * stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t alpha = bgra[(static_cast<size_t>(y) * width + x) * 4u + 3u];
            if (alpha == 0) {
                row[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
            }
        }
    }
    return CreateBitmap(width, height, 1, 1, bits.data());
}

// Point sampling of premultiplied BGRA. Cursors are small and their edges are
// already anti-aliased, so filtering buys nothing at these factors.
void resize_bgra(const std::vector<uint8_t>& src, int src_w, int src_h, int dst_w, int dst_h,
                 std::vector<uint8_t>& dst) {
    dst.assign(static_cast<size_t>(dst_w) * dst_h * 4u, 0);
    for (int y = 0; y < dst_h; ++y) {
        const int sy = (std::min)(src_h - 1, y * src_h / dst_h);
        for (int x = 0; x < dst_w; ++x) {
            const int sx = (std::min)(src_w - 1, x * src_w / dst_w);
            std::memcpy(dst.data() + (static_cast<size_t>(y) * dst_w + x) * 4u,
                        src.data() + (static_cast<size_t>(sy) * src_w + sx) * 4u, 4u);
        }
    }
}

int scale_percent(double scale) {
    return static_cast<int>(std::lround(RemoteCursor::QuantiseScale(scale) * 100.0));
}

}  // namespace

RemoteCursor::~RemoteCursor() {
    Clear();
}

double RemoteCursor::QuantiseScale(double scale) {
    if (!(scale > 0.0)) {
        return 1.0;
    }
    const double clamped = std::clamp(scale, 0.5, 4.0);
    if (clamped < 2.0) {
        return std::round(clamped * 4.0) / 4.0;
    }
    return std::round(clamped);
}

bool RemoteCursor::Register(uint64_t shape_id, int width, int height, int hotspot_x,
                            int hotspot_y, const std::vector<uint8_t>& bgra,
                            std::string* error) {
    if (shape_id == 0 || width <= 0 || height <= 0 ||
        width > kMaxCursorSide || height > kMaxCursorSide ||
        bgra.size() != static_cast<size_t>(width) * height * 4u ||
        bgra.size() > kMaxShapeBytes) {
        if (error) {
            *error = "the cursor sprite does not match its size";
        }
        return false;
    }
    if (shapes_.contains(shape_id)) {
        return true;
    }
    Shape shape;
    shape.width = width;
    shape.height = height;
    shape.hotspot_x = std::clamp(hotspot_x, 0, width - 1);
    shape.hotspot_y = std::clamp(hotspot_y, 0, height - 1);
    shape.bgra = bgra;
    shapes_.emplace(shape_id, std::move(shape));
    order_.push_back(shape_id);
    while (shapes_.size() > kMaxShapes) {
        evict_locked();
    }
    return true;
}

void RemoteCursor::evict_locked() {
    while (!order_.empty()) {
        const uint64_t id = order_.front();
        order_.pop_front();
        const auto found = shapes_.find(id);
        if (found == shapes_.end()) {
            continue;
        }
        if (id == active_shape_) {
            // The sprite the OS is showing is about to be freed; drop the
            // override rather than leave the cursor pointing at a dead handle.
            Deactivate();
        }
        release_shape(found->second);
        shapes_.erase(found);
        return;
    }
}

void RemoteCursor::release_shape(Shape& shape) {
    for (auto& scaled : shape.scaled) {
        // DestroyCursor, not DeleteObject: measured in RustDesk's engine
        // work, DeleteObject leaves a cursor made by CreateIconIndirect
        // alive. DestroyCursor is tried first, and DeleteObject catches a
        // handle of the other kind.
        if (!DestroyCursor(scaled.second)) {
            DeleteObject(scaled.second);
        }
    }
    shape.scaled.clear();
}

bool RemoteCursor::Activate(uint64_t shape_id, double scale, std::string* error) {
    const auto found = shapes_.find(shape_id);
    if (found == shapes_.end()) {
        if (error) {
            *error = "the cursor shape was never registered";
        }
        return false;
    }
    const Shape& shape = found->second;
    int percent = scale_percent(scale);
    int width = (std::max)(1, static_cast<int>(
                                   std::lround(shape.width * percent / 100.0)));
    int height = (std::max)(1, static_cast<int>(
                                    std::lround(shape.height * percent / 100.0)));
    if (width > kMaxCursorSide || height > kMaxCursorSide) {
        // Shrink to fit rather than fail: a slightly small pointer beats none.
        const double fit = (std::min)(static_cast<double>(kMaxCursorSide) / shape.width,
                                      static_cast<double>(kMaxCursorSide) / shape.height);
        percent = scale_percent(fit);
        width = (std::max)(1, static_cast<int>(
                                  std::lround(shape.width * percent / 100.0)));
        height = (std::max)(1, static_cast<int>(
                                   std::lround(shape.height * percent / 100.0)));
    }
    if (!shape.scaled.contains(percent)) {
        if (shape.scaled.size() >= kMaxScalesPerShape) {
            // QuantiseScale only yields a handful of factors; reaching this
            // means something called Activate with a raw scale. Drop the
            // smallest factor rather than grow without bound.
            auto oldest = shape.scaled.begin();
            if (!DestroyCursor(oldest->second)) {
                DeleteObject(oldest->second);
            }
            found->second.scaled.erase(oldest);
        }
        HCURSOR built = build_locked(shape, width, height, percent, error);
        if (built == nullptr) {
            return false;
        }
        found->second.scaled.emplace(percent, built);
    }
    active_shape_ = shape_id;
    active_scale_ = percent;
    return true;
}

HCURSOR RemoteCursor::build_locked(const Shape& shape, int width, int height, int percent,
                                   std::string* error) {
    std::vector<uint8_t> scaled;
    const std::vector<uint8_t>* pixels = &shape.bgra;
    if (width != shape.width || height != shape.height) {
        resize_bgra(shape.bgra, shape.width, shape.height, width, height, scaled);
        pixels = &scaled;
    }

    HDC dc = GetDC(nullptr);
    if (!dc) {
        if (error) {
            *error = "GetDC failed";
        }
        return nullptr;
    }
    HBITMAP color = build_color_bitmap(dc, width, height, *pixels);
    HBITMAP mask = build_mask_bitmap(width, height, *pixels);
    ReleaseDC(nullptr, dc);
    if (!color || !mask) {
        if (color) {
            DeleteObject(color);
        }
        if (mask) {
            DeleteObject(mask);
        }
        if (error) {
            *error = "the cursor bitmaps could not be created";
        }
        return nullptr;
    }

    const double factor = percent / 100.0;
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(std::clamp<long>(
        std::lround(shape.hotspot_x * factor), 0, width - 1));
    info.yHotspot = static_cast<DWORD>(std::clamp<long>(
        std::lround(shape.hotspot_y * factor), 0, height - 1));
    info.hbmMask = mask;
    info.hbmColor = color;
    HCURSOR cursor = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    if (!cursor && error) {
        *error = "CreateIconIndirect failed";
    }
    return cursor;
}

void RemoteCursor::Deactivate() {
    active_shape_ = 0;
    active_scale_ = 0;
}

HCURSOR RemoteCursor::cursor() const {
    const auto shape = shapes_.find(active_shape_);
    if (shape == shapes_.end()) {
        return nullptr;
    }
    const auto scaled = shape->second.scaled.find(active_scale_);
    return scaled == shape->second.scaled.end() ? nullptr : scaled->second;
}

void RemoteCursor::Clear() {
    for (auto& shape : shapes_) {
        release_shape(shape.second);
    }
    shapes_.clear();
    order_.clear();
    Deactivate();
}

}  // namespace chast_viewer
