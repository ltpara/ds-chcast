#include "capture/rotation.h"

#include <algorithm>
#include <cstring>
#include <limits>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace chast::detail {

UINT rotation_degrees(DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: return 90;
        case DXGI_MODE_ROTATION_ROTATE180: return 180;
        case DXGI_MODE_ROTATION_ROTATE270: return 270;
        default: return 0;
    }
}

bool swaps_axes(DXGI_MODE_ROTATION rotation) {
    return rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
}

POINT surface_to_output(const POINT& p, UINT width, UINT height, DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
        // Top edge of the panel becomes the left edge of the output.
        case DXGI_MODE_ROTATION_ROTATE90:
            return POINT{static_cast<LONG>(height) - 1 - p.y, p.x};
        case DXGI_MODE_ROTATION_ROTATE180:
            return POINT{static_cast<LONG>(width) - 1 - p.x, static_cast<LONG>(height) - 1 - p.y};
        case DXGI_MODE_ROTATION_ROTATE270:
            return POINT{p.y, static_cast<LONG>(width) - 1 - p.x};
        default:
            return p;
    }
}

RECT surface_to_output(const RECT& r, UINT width, UINT height, DXGI_MODE_ROTATION rotation) {
    // Transform the two opposite corners and re-normalize: the transform is an
    // axis-aligned flip/transpose, so corners stay on the rect's boundary.
    POINT a = surface_to_output(POINT{r.left, r.top}, width, height, rotation);
    POINT b = surface_to_output(POINT{r.right - 1, r.bottom - 1}, width, height, rotation);
    RECT out;
    out.left = std::min(a.x, b.x);
    out.top = std::min(a.y, b.y);
    out.right = std::max(a.x, b.x) + 1;
    out.bottom = std::max(a.y, b.y) + 1;
    return out;
}

D3D11_BOX destination_box(UINT x, UINT y, UINT width, UINT height) {
    return D3D11_BOX{x, y, 0, x + width, y + height, 1};
}

bool rotate_bgra(const uint8_t* source, UINT source_pitch, UINT width, UINT height,
                 DXGI_MODE_ROTATION rotation, std::vector<uint8_t>* output,
                 UINT* output_width, UINT* output_height) {
    if (!source || !output || !output_width || !output_height || width == 0 || height == 0 ||
        static_cast<uint64_t>(source_pitch) < static_cast<uint64_t>(width) * 4) {
        return false;
    }
    if (rotation != DXGI_MODE_ROTATION_IDENTITY &&
        rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
        rotation != DXGI_MODE_ROTATION_ROTATE90 &&
        rotation != DXGI_MODE_ROTATION_ROTATE180 &&
        rotation != DXGI_MODE_ROTATION_ROTATE270) {
        return false;
    }

    const UINT out_width = swaps_axes(rotation) ? height : width;
    const UINT out_height = swaps_axes(rotation) ? width : height;
    const uint64_t out_pitch = static_cast<uint64_t>(out_width) * 4;
    const uint64_t out_size = out_pitch * out_height;
    if (out_size > std::numeric_limits<size_t>::max()) {
        return false;
    }
    output->resize(static_cast<size_t>(out_size));

    for (UINT y = 0; y < out_height; ++y) {
        uint8_t* dst = output->data() + static_cast<size_t>(y) * static_cast<size_t>(out_pitch);
        if (rotation == DXGI_MODE_ROTATION_IDENTITY ||
            rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
            std::memcpy(dst, source + static_cast<size_t>(y) * source_pitch,
                        static_cast<size_t>(out_pitch));
            continue;
        }
        for (UINT x = 0; x < out_width; ++x) {
            UINT source_x = 0;
            UINT source_y = 0;
            switch (rotation) {
                case DXGI_MODE_ROTATION_ROTATE90:
                    source_x = y;
                    source_y = height - 1 - x;
                    break;
                case DXGI_MODE_ROTATION_ROTATE180:
                    source_x = width - 1 - x;
                    source_y = height - 1 - y;
                    break;
                case DXGI_MODE_ROTATION_ROTATE270:
                    source_x = width - 1 - y;
                    source_y = x;
                    break;
                default:
                    break;
            }
            std::memcpy(dst + static_cast<size_t>(x) * 4,
                        source + static_cast<size_t>(source_y) * source_pitch +
                            static_cast<size_t>(source_x) * 4,
                        4);
        }
    }

    *output_width = out_width;
    *output_height = out_height;
    return true;
}

}  // namespace chast::detail
