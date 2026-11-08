#pragma once

#include <algorithm>
#include "chast/capture.h"

namespace chast {
inline bool same_display_identity(const DisplayInfo& a, const DisplayInfo& b) {
    return a.device_name == b.device_name &&
        a.adapter_luid.HighPart == b.adapter_luid.HighPart &&
        a.adapter_luid.LowPart == b.adapter_luid.LowPart;
}
inline bool same_display_geometry(const DisplayInfo& a, const DisplayInfo& b) {
    return a.rect.left == b.rect.left && a.rect.top == b.rect.top &&
        a.rect.right == b.rect.right && a.rect.bottom == b.rect.bottom &&
        a.width == b.width && a.height == b.height && a.rotation_degrees == b.rotation_degrees &&
        a.hdr == b.hdr && a.advanced_color_sdr == b.advanced_color_sdr &&
        a.sdr_white_level == b.sdr_white_level;
}
inline bool same_display_layout(const std::vector<DisplayInfo>& a, const std::vector<DisplayInfo>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
        return same_display_identity(x, y) && same_display_geometry(x, y) && x.primary == y.primary;
    });
}
} // namespace chast
