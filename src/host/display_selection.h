#pragma once

#include <algorithm>
#include "chast/protocol.h"

namespace chast {

// RustDesk Connection::capture_displays + Server::capture_displays:
// add wins over sub, which wins over set. sub is an intersection with the
// current subscriptions; it does not create a service or subtract its list.
// An entirely invalid nonempty operation is ignored. Empty set unsubscribes all.
inline std::vector<uint16_t> select_capture_displays(std::vector<uint16_t> current,
        const CaptureDisplaysRequest& request, size_t source_count) {
    const auto& requested = !request.add.empty() ? request.add :
        !request.sub.empty() ? request.sub : request.set;
    std::vector<uint16_t> valid;
    for (auto index : requested)
        if (index < source_count && index < kMaxDisplayCount) valid.push_back(index);
    std::sort(current.begin(), current.end());
    current.erase(std::unique(current.begin(), current.end()), current.end());
    if (!requested.empty() && valid.empty()) return current;
    if (!request.add.empty()) {
        valid.insert(valid.end(), current.begin(), current.end());
    } else if (!request.sub.empty()) {
        valid.erase(std::remove_if(valid.begin(), valid.end(), [&](auto index) {
            return !std::binary_search(current.begin(), current.end(), index);
        }), valid.end());
    }
    std::sort(valid.begin(), valid.end());
    valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
    return valid;
}

} // namespace chast
