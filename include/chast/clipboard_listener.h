#pragma once

#include <cstdint>
#include <functional>

namespace chast {

// One message-only window per process receives WM_CLIPBOARDUPDATE for every
// session, so a host with several viewers (and a viewer with several windows)
// shares one native listener instead of polling the clipboard per session.
// Subscribers react to Generation() changes; callbacks run on the listener
// thread and must be quick.
class ClipboardListener {
public:
    // Starts the shared listener on first use. Returns 0 when this process
    // cannot own a clipboard listener (for example a service without an
    // interactive window station), or startup exceeds two seconds. Callers
    // keep their timer fallback then. A delayed worker is cancelled and reaped
    // on a later Subscribe; repeated retries never create additional workers.
    static uint64_t Subscribe(std::function<void()> changed = {});
    static void Unsubscribe(uint64_t subscription);

    // Monotonic counter bumped for every notification, including empty and
    // unsupported clipboard contents.
    static uint64_t Generation();
    static bool Running();
};

} // namespace chast
