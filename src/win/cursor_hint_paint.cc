#include "win/cursor_hint_paint.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace chast {

namespace {

constexpr int kDesignSize = 32;
constexpr int kSamplesPerAxis = 4;
constexpr double kOutlineWidth = 1.4;
constexpr int kHintAlpha = 235;

constexpr int kRippleMinDesign = 4;
constexpr int kRippleMaxDesign = 18;
constexpr int kRippleAlpha = 150;

constexpr int kLabelGapXDesign = 24;
constexpr int kLabelGapYDesign = 22;
constexpr int kLabelAlpha = 235;
constexpr int kShadowAlpha = 160;

// Design-space arrow polygon, tip first, in the 32x32 cursor grid Windows
// uses at 100% scaling.
constexpr double kArrow[7][2] = {
    {1.5, 1.0},  {1.5, 23.0}, {7.0, 17.5}, {11.0, 27.5},
    {15.5, 25.5}, {11.5, 16.0}, {19.0, 16.0},
};

bool point_in_polygon(double x, double y) {
    constexpr int kVertices = 7;
    bool inside = false;
    for (int i = 0, j = kVertices - 1; i < kVertices; j = i++) {
        const double xi = kArrow[i][0];
        const double yi = kArrow[i][1];
        const double xj = kArrow[j][0];
        const double yj = kArrow[j][1];
        if (((yi > y) != (yj > y)) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }
    return inside;
}

double segment_distance(double x, double y, double x1, double y1, double x2, double y2) {
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double length_sq = dx * dx + dy * dy;
    double t = length_sq > 0.0 ? ((x - x1) * dx + (y - y1) * dy) / length_sq : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const double offset_x = x1 + t * dx - x;
    const double offset_y = y1 + t * dy - y;
    return std::sqrt(offset_x * offset_x + offset_y * offset_y);
}

double outline_distance(double x, double y) {
    constexpr int kVertices = 7;
    double nearest = segment_distance(x, y, kArrow[0][0], kArrow[0][1], kArrow[1][0],
                                      kArrow[1][1]);
    for (int i = 1; i < kVertices; ++i) {
        const int j = (i + 1) % kVertices;
        const double distance =
            segment_distance(x, y, kArrow[i][0], kArrow[i][1], kArrow[j][0], kArrow[j][1]);
        if (distance < nearest) {
            nearest = distance;
        }
    }
    return nearest;
}

// Source-over with a premultiplied source, so a ring or a glyph can sit on
// top of another translucent shape without darkening its edges.
void blend(uint32_t* pixel, uint32_t alpha, uint32_t red, uint32_t green, uint32_t blue) {
    if (alpha == 0) {
        return;
    }
    const uint32_t dst = *pixel;
    const uint32_t keep = 255 - alpha;
    const uint32_t dst_alpha = dst >> 24;
    const uint32_t out_alpha = alpha + dst_alpha * keep / 255;
    const uint32_t out_red = red + ((dst >> 16) & 0xFFu) * keep / 255;
    const uint32_t out_green = green + ((dst >> 8) & 0xFFu) * keep / 255;
    const uint32_t out_blue = blue + (dst & 0xFFu) * keep / 255;
    *pixel = (out_alpha << 24) | (out_red << 16) | (out_green << 8) | out_blue;
}

uint32_t premultiply(uint32_t channel, uint32_t alpha) {
    return channel * alpha / 255;
}

void paint_ripple(uint32_t* pixels, const CursorHintSurface& surface) {
    if (surface.ripple < 0.0) {
        return;
    }
    const double radius = cursor_hint_ripple_radius(surface.arrow_width, surface.ripple);
    const uint32_t alpha = static_cast<uint32_t>(cursor_hint_ripple_alpha(surface.ripple));
    const double thickness = std::max(1.5, surface.arrow_width * 2.4 / kDesignSize);
    const int reach = static_cast<int>(std::ceil(radius + thickness)) + 2;
    const uint32_t red = (surface.color >> 16) & 0xFFu;
    const uint32_t green = (surface.color >> 8) & 0xFFu;
    const uint32_t blue = surface.color & 0xFFu;
    const int left = std::max(0, surface.tip_x - reach);
    const int top = std::max(0, surface.tip_y - reach);
    const int right = std::min(surface.width, surface.tip_x + reach + 1);
    const int bottom = std::min(surface.height, surface.tip_y + reach + 1);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const double dx = x + 0.5 - surface.tip_x;
            const double dy = y + 0.5 - surface.tip_y;
            const double distance = std::sqrt(dx * dx + dy * dy);
            const double coverage =
                std::clamp(thickness * 0.5 + 0.5 - std::fabs(distance - radius), 0.0, 1.0);
            if (coverage <= 0.0) {
                continue;
            }
            const uint32_t pixel_alpha = static_cast<uint32_t>(alpha * coverage + 0.5);
            blend(&pixels[y * surface.width + x], pixel_alpha, premultiply(red, pixel_alpha),
                  premultiply(green, pixel_alpha), premultiply(blue, pixel_alpha));
        }
    }
}

void paint_label(uint32_t* pixels, const CursorHintSurface& surface) {
    if (surface.label_mask == nullptr || surface.label_width <= 0 ||
        surface.label_height <= 0) {
        return;
    }
    const uint32_t red = (surface.color >> 16) & 0xFFu;
    const uint32_t green = (surface.color >> 8) & 0xFFu;
    const uint32_t blue = surface.color & 0xFFu;
    const auto coverage = [&surface](int x, int y) -> uint32_t {
        return surface.label_mask[y * surface.label_width + x];
    };
    // Shadow first, one pixel down and right, so the glyph stays readable on a
    // light wallpaper.
    for (int y = 0; y < surface.label_height; ++y) {
        for (int x = 0; x < surface.label_width; ++x) {
            const uint32_t glyph = coverage(x, y);
            if (glyph == 0) {
                continue;
            }
            const int px = surface.label_x + x + 1;
            const int py = surface.label_y + y + 1;
            if (px >= surface.width || py >= surface.height) {
                continue;
            }
            const uint32_t alpha = glyph * kShadowAlpha / 255;
            blend(&pixels[py * surface.width + px], alpha, 0, 0, 0);
        }
    }
    for (int y = 0; y < surface.label_height; ++y) {
        for (int x = 0; x < surface.label_width; ++x) {
            const uint32_t glyph = coverage(x, y);
            if (glyph == 0) {
                continue;
            }
            const int px = surface.label_x + x;
            const int py = surface.label_y + y;
            if (px >= surface.width || py >= surface.height) {
                continue;
            }
            const uint32_t alpha = glyph * kLabelAlpha / 255;
            blend(&pixels[py * surface.width + px], alpha, premultiply(red, alpha),
                  premultiply(green, alpha), premultiply(blue, alpha));
        }
    }
}

void paint_arrow(uint32_t* pixels, const CursorHintSurface& surface) {
    constexpr int kSamples = kSamplesPerAxis * kSamplesPerAxis;
    const uint32_t red = (surface.color >> 16) & 0xFFu;
    const uint32_t green = (surface.color >> 8) & 0xFFu;
    const uint32_t blue = surface.color & 0xFFu;
    const double scale_x = static_cast<double>(kDesignSize) / surface.arrow_width;
    const double scale_y = static_cast<double>(kDesignSize) / surface.arrow_height;
    const int left = std::max(0, surface.arrow_x);
    const int top = std::max(0, surface.arrow_y);
    const int right = std::min(surface.width, surface.arrow_x + surface.arrow_width);
    const int bottom = std::min(surface.height, surface.arrow_y + surface.arrow_height);
    for (int py = top; py < bottom; ++py) {
        for (int px = left; px < right; ++px) {
            int covered = 0;
            int filled = 0;
            for (int sy = 0; sy < kSamplesPerAxis; ++sy) {
                for (int sx = 0; sx < kSamplesPerAxis; ++sx) {
                    const double x =
                        (px - surface.arrow_x + (sx + 0.5) / kSamplesPerAxis) * scale_x;
                    const double y =
                        (py - surface.arrow_y + (sy + 0.5) / kSamplesPerAxis) * scale_y;
                    if (!point_in_polygon(x, y)) {
                        continue;
                    }
                    ++covered;
                    if (outline_distance(x, y) >= kOutlineWidth) {
                        ++filled;
                    }
                }
            }
            if (covered == 0) {
                continue;
            }
            // Premultiplied BGRA: the viewer's colour inside a black stroke.
            const uint32_t alpha = static_cast<uint32_t>(kHintAlpha) * covered / kSamples;
            const uint32_t fill = static_cast<uint32_t>(kHintAlpha) * filled / kSamples;
            blend(&pixels[py * surface.width + px], alpha, premultiply(red, fill),
                  premultiply(green, fill), premultiply(blue, fill));
        }
    }
}

}  // namespace

int cursor_hint_margin(int arrow_width) {
    const int scaled = std::max(1, arrow_width) * kRippleMaxDesign / kDesignSize;
    return scaled + 2;
}

double cursor_hint_ripple_radius(int arrow_width, double progress) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    const double design = kRippleMinDesign + (kRippleMaxDesign - kRippleMinDesign) * clamped;
    return design * std::max(1, arrow_width) / kDesignSize;
}

int cursor_hint_ripple_alpha(double progress) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return static_cast<int>(kRippleAlpha * (1.0 - clamped) + 0.5);
}

uint32_t cursor_hint_color(const std::string& label) {
    if (label.empty()) {
        return 0xFFFFFF;
    }
    // Saturated mid-tones read on both light and dark wallpapers.
    static constexpr uint32_t kPalette[] = {
        0xE4572E, 0x2E86AB, 0x6A994E, 0xBC4B51, 0x7B2CBF,
        0xE09F3E, 0x1B998B, 0xC2185B, 0x3D5A80, 0x9A031E,
    };
    uint32_t hash = 2166136261u;
    for (const unsigned char byte : label) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return kPalette[hash % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

CursorHintLayout cursor_hint_layout(int arrow_width, int arrow_height, int hotspot_x,
                                    int hotspot_y, int label_width, int label_height) {
    CursorHintLayout layout;
    const int margin = cursor_hint_margin(arrow_width);
    layout.arrow_x = margin;
    layout.arrow_y = margin;
    layout.tip_x = margin + hotspot_x;
    layout.tip_y = margin + hotspot_y;
    layout.label_width = std::max(0, label_width);
    layout.label_height = std::max(0, label_height);
    if (layout.label_width > 0 && layout.label_height > 0) {
        layout.label_x = layout.tip_x + kLabelGapXDesign * std::max(1, arrow_width) / kDesignSize;
        layout.label_y = layout.tip_y + kLabelGapYDesign * std::max(1, arrow_width) / kDesignSize;
    }
    int right = layout.arrow_x + std::max(0, arrow_width);
    int bottom = layout.arrow_y + std::max(0, arrow_height);
    if (layout.label_width > 0 && layout.label_height > 0) {
        right = std::max(right, layout.label_x + layout.label_width);
        bottom = std::max(bottom, layout.label_y + layout.label_height);
    }
    layout.width = right + margin;
    layout.height = bottom + margin;
    return layout;
}

void paint_cursor_hint(uint32_t* pixels, const CursorHintSurface& surface) {
    if (pixels == nullptr || surface.width <= 0 || surface.height <= 0) {
        return;
    }
    const std::size_t count = static_cast<std::size_t>(surface.width) * surface.height;
    for (std::size_t i = 0; i < count; ++i) {
        pixels[i] = 0;
    }
    paint_ripple(pixels, surface);
    paint_label(pixels, surface);
    paint_arrow(pixels, surface);
}

}  // namespace chast
