// Geometry of rotated outputs.
//
// Desktop Duplication returns the desktop image in the display's *native*
// panel orientation, while DXGI_OUTDUPL_DESC::ModeDesc reports the rotated
// (screen-space) mode. On a portrait panel rotated 90 degrees the surface is
// landscape, ModeDesc is portrait, and the contents come back rotated. Every
// rectangle DXGI reports (dirty regions, move regions, pointer position) is in
// surface coordinates, so once the rotation is applied on the GPU the regions
// have to be mapped into the same space as the published texture.
//
// rotation of 0/180 keep the surface and screen axes aligned; 90/270 swap
// them. `surface_to_output` implements the inverse of the rotation the
// compositor applied, i.e. the transform from the raw surface to what a
// screenshot of the rotated desktop looks like.
#pragma once

#include <windows.h>

#include <dxgi.h>

namespace chast::detail {

UINT rotation_degrees(DXGI_MODE_ROTATION rotation);

bool swaps_axes(DXGI_MODE_ROTATION rotation);

// Size of the raw duplication surface for a screen-space mode size.
inline void surface_size(UINT mode_width, UINT mode_height, DXGI_MODE_ROTATION rotation,
                         UINT& out_width, UINT& out_height) {
    if (swaps_axes(rotation)) {
        out_width = mode_height;
        out_height = mode_width;
    } else {
        out_width = mode_width;
        out_height = mode_height;
    }
}

// Maps a point from surface space to screen space. `width`/`height` are the
// raw surface dimensions.
POINT surface_to_output(const POINT& point, UINT width, UINT height, DXGI_MODE_ROTATION rotation);

// Maps a rect from surface space to screen space. Corners stay on the rect's
// boundary because the transform is an axis-aligned flip/transpose, so the
// result is exact and needs no padding.
RECT surface_to_output(const RECT& rect, UINT width, UINT height, DXGI_MODE_ROTATION rotation);

}  // namespace chast::detail
