#pragma once

#include <string_view>

namespace chast_viewer {

enum class DisplayWindowAccess { Allowed, Unsupported, SessionEnded };

// Display windows borrow the parent's engine. They may operate its media but
// cannot replace, authenticate or stop the connection. Old windows must not
// change the media of a reconnected session.
inline DisplayWindowAccess display_window_access(std::string_view method,
                                                 bool current_generation) {
    const bool cleanup = method == "bindDisplayOutput" || method == "releaseInput" ||
                         method == "setKeyCapture" || method == "closeSessionWindow";
    const bool allowed = cleanup || method == "displayStreams" ||
        method == "takeDisplayCursors" || method == "focusDisplay" ||
        method == "status" || method == "sendInput" || method == "keyboardState" ||
        method == "setKeyboardMode" || method == "setFullscreen" ||
        method == "setAudioMuted" || method == "setMicrophone" ||
        method == "setVoiceCall" || method == "setRecording" ||
        method == "listAudioOutputs" || method == "selectAudioOutput" ||
        method == "listMicrophones" || method == "selectMicrophone" ||
        method == "setAudioProcessing";
    if (!allowed) return DisplayWindowAccess::Unsupported;
    if (!current_generation && !cleanup) return DisplayWindowAccess::SessionEnded;
    return DisplayWindowAccess::Allowed;
}

}  // namespace chast_viewer
