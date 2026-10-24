#pragma once

// Privacy mode: a black, click-through layer over the virtual screen that is
// excluded from screen capture. The local monitor (or an onlooker) sees black;
// DXGI duplication still sees the desktop underneath, so the remote viewer
// keeps working.
//
// The exclusion uses SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE), which
// exists from Windows 10 version 2004 (build 19041). Without it the mask would
// also be captured, which defeats the feature, so create() refuses rather than
// showing a layer that would black out the viewer too.
//
// The window lives on the Default desktop: while a secure desktop (lock
// screen, UAC) is up it is simply not visible, and it is already in place when
// the user returns. It never takes focus and never blocks local input.

#include <memory>
#include <string>

namespace chast {

// True when the OS can hide a window from capture (Windows 10 2004+).
bool privacy_mode_supported();

class PrivacyOverlay {
public:
    // Starts the window thread and proves the capture exclusion works.
    // Returns nullptr with *error set when the mask cannot be provided.
    static std::unique_ptr<PrivacyOverlay> create(std::string* error);

    ~PrivacyOverlay();
    PrivacyOverlay(const PrivacyOverlay&) = delete;
    PrivacyOverlay& operator=(const PrivacyOverlay&) = delete;

    // Shows the mask over the whole virtual screen. Returns false and fills
    // *error when the window or the placement failed; enabled() then stays
    // false.
    bool enable(std::string* error);

    // Hides the mask. Safe to call when it is already hidden.
    void disable();

    bool enabled() const;

private:
    PrivacyOverlay();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
