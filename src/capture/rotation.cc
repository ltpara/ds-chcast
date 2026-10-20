#include "capture/rotation.h"

#include <algorithm>

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

}  // namespace chast::detail
