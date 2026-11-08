#pragma once
#include "chast/host_server.h"
#include <functional>

namespace chast::host {
// Both the in-process host and the authenticated service channel expose the
// same explicit-target operations. A successful remote action means queued;
// its eventual refusal is delivered to the selected conversation.
struct ConnectionActions {
    std::function<std::vector<HostViewer>()> viewers;
    std::function<bool(uint64_t, PermissionMask)> permissions;
    std::function<bool(uint64_t, const std::string&)> chat;
    std::function<bool(uint64_t, const std::string&, std::string*)> file;
    std::function<bool(uint64_t)> disconnect;
};
}
