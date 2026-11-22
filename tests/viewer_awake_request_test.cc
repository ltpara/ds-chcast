#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "viewer_awake_request.h"

namespace {

bool test_request_policy() {
    using chast_viewer::ViewerAwakeRequestAction;
    using chast_viewer::viewer_awake_request_action;
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };
    check(viewer_awake_request_action(true, false, true) ==
              ViewerAwakeRequestAction::Acquire,
          "an active session can acquire its first lease");
    check(viewer_awake_request_action(true, false, false) ==
              ViewerAwakeRequestAction::Reject,
          "a late request cannot acquire after its session ended");
    check(viewer_awake_request_action(true, true, false) ==
              ViewerAwakeRequestAction::Noop,
          "an existing lease is not double-counted");
    check(viewer_awake_request_action(false, true, false) ==
              ViewerAwakeRequestAction::Release,
          "an ended session can still release its held lease");
    check(viewer_awake_request_action(false, false, true) ==
              ViewerAwakeRequestAction::Noop,
          "a release without a held lease is a no-op");
    return failures == 0;
}

}  // namespace

int main(int argc, char** argv) {
    int hold_ms = 0;
    if (argc != 1) {
        if (argc != 3 || std::strcmp(argv[1], "--hold-ms") != 0) {
            std::fprintf(stderr, "usage: viewer_awake_request_test [--hold-ms N]\n");
            return 2;
        }
        const char* const end = argv[2] + std::strlen(argv[2]);
        const auto parsed = std::from_chars(argv[2], end, hold_ms);
        if (parsed.ec != std::errc{} || parsed.ptr != end || hold_ms < 0 ||
            hold_ms > 30000) {
            std::fprintf(stderr, "hold duration must be between 0 and 30000 ms\n");
            return 2;
        }
    }

    if (!test_request_policy()) return 1;

    if (!chast_viewer::AcquireViewerAwakeRequest()) {
        std::fprintf(stderr, "could not acquire the Windows awake request\n");
        return 1;
    }
    if (!chast_viewer::AcquireViewerAwakeRequest()) {
        (void)chast_viewer::ReleaseViewerAwakeRequest();
        std::fprintf(stderr, "could not acquire the second session lease\n");
        return 1;
    }

    std::puts("awake request active for two session leases");
    std::fflush(stdout);
    if (hold_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    }

    if (!chast_viewer::ReleaseViewerAwakeRequest()) {
        std::fprintf(stderr, "could not release the first session lease\n");
        return 1;
    }
    if (!chast_viewer::ReleaseViewerAwakeRequest()) {
        std::fprintf(stderr, "could not clear the Windows awake request\n");
        return 1;
    }
    std::puts("all awake request leases released");
    return 0;
}
