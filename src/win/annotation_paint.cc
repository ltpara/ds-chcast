#include "win/annotation_paint.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace chast {

namespace {

void blend_pixel(uint32_t* pixel, uint32_t alpha, uint32_t red, uint32_t green,
                 uint32_t blue) {
    if (alpha == 0) {
        return;
    }
    const uint32_t keep = 255u - alpha;
    const uint32_t dst = *pixel;
    const uint32_t out_alpha = alpha + (dst >> 24) * keep / 255u;
    const uint32_t out_red = red + ((dst >> 16) & 0xFFu) * keep / 255u;
    const uint32_t out_green = green + ((dst >> 8) & 0xFFu) * keep / 255u;
    const uint32_t out_blue = blue + (dst & 0xFFu) * keep / 255u;
    *pixel = (out_alpha << 24) | (out_red << 16) | (out_green << 8) | out_blue;
}

double distance_to_segment(double x, double y, double x0, double y0, double x1,
                           double y1) {
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double length_sq = dx * dx + dy * dy;
    double t = length_sq > 0.0 ? ((x - x0) * dx + (y - y0) * dy) / length_sq : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double offset_x = x0 + t * dx - x;
    const double offset_y = y0 + t * dy - y;
    return std::sqrt(offset_x * offset_x + offset_y * offset_y);
}

}  // namespace

void annotation_draw_segment(uint32_t* pixels, int width, int height, int x0, int y0,
                             int x1, int y1, double radius, uint32_t color,
                             uint8_t alpha) {
    if (pixels == nullptr || width <= 0 || height <= 0 || radius <= 0.0 || alpha == 0) {
        return;
    }
    const int reach = static_cast<int>(std::ceil(radius + 1.0));
    const int left = std::max(0, std::min(x0, x1) - reach);
    const int top = std::max(0, std::min(y0, y1) - reach);
    const int right = std::min(width, std::max(x0, x1) + reach + 1);
    const int bottom = std::min(height, std::max(y0, y1) + reach + 1);
    if (left >= right || top >= bottom) {
        return;
    }
    const uint32_t red = (color >> 16) & 0xFFu;
    const uint32_t green = (color >> 8) & 0xFFu;
    const uint32_t blue = color & 0xFFu;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const double distance =
                distance_to_segment(x + 0.5, y + 0.5, x0, y0, x1, y1);
            const double coverage = std::clamp(radius + 0.75 - distance, 0.0, 1.0);
            if (coverage <= 0.0) {
                continue;
            }
            const uint32_t pixel_alpha =
                static_cast<uint32_t>(static_cast<double>(alpha) * coverage + 0.5);
            const uint32_t premultiplied_red = red * pixel_alpha / 255u;
            const uint32_t premultiplied_green = green * pixel_alpha / 255u;
            const uint32_t premultiplied_blue = blue * pixel_alpha / 255u;
            blend_pixel(pixels + static_cast<size_t>(y) * width + x, pixel_alpha,
                        premultiplied_red, premultiplied_green, premultiplied_blue);
        }
    }
}

}  // namespace chast
