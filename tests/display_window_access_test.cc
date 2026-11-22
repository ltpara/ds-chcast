#include <cstdio>
#include <initializer_list>
#include <string_view>

#include "display_window_access.h"

int main() {
    using chast_viewer::DisplayWindowAccess;
    using chast_viewer::display_window_access;
    int failures = 0;
    const auto check = [&](std::string_view method, bool current,
                           DisplayWindowAccess expected) {
        if (display_window_access(method, current) != expected) {
            std::fprintf(stderr, "%.*s: unexpected access (current=%d)\n",
                         static_cast<int>(method.size()), method.data(), current);
            ++failures;
        }
    };
    for (const auto method : {"setAudioMuted", "setMicrophone", "setVoiceCall",
             "setRecording", "listAudioOutputs", "selectAudioOutput",
             "listMicrophones", "selectMicrophone", "setAudioProcessing",
             "status", "sendInput", "focusDisplay"}) {
        check(method, true, DisplayWindowAccess::Allowed);
        check(method, false, DisplayWindowAccess::SessionEnded);
    }
    for (const auto method : {"start", "stop", "authenticate", "openSessionWindow",
             "openDisplayWindow", "controlDisplays", "unknown"}) {
        check(method, true, DisplayWindowAccess::Unsupported);
        check(method, false, DisplayWindowAccess::Unsupported);
    }
    for (const auto method : {"bindDisplayOutput", "releaseInput", "setKeyCapture",
             "closeSessionWindow"}) {
        check(method, true, DisplayWindowAccess::Allowed);
        check(method, false, DisplayWindowAccess::Allowed);
    }
    return failures ? 1 : 0;
}
