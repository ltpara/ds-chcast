#pragma once

// Selecting a display mode (resolution and refresh rate) of one output.
//
// This is the Win32 mode layer, separate from capture: a viewer picks a mode
// the output reports, the host changes it for the session only and puts the
// original back when the session ends. Nothing here touches DXGI, and a mode
// change is expected to cost the duplication its surface; the caller rebuilds
// capture through IDxgiCapturer::reinitialize().
//
// Widths and heights are in desktop space, the orientation the user sees: a
// panel rotated 90 or 270 reports the swapped pair, the same space
// DisplayInfo::width/height and the viewer's picture use. The native
// orientation is reconstructed inside apply()/restore().

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/capture.h"

namespace chast {

struct DisplayMode {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t refresh_hz = 0;
    // True for the mode the output runs right now.
    bool current = false;
};

// The wire protocol bounds a mode list to 256 entries; enumeration stops at
// the same point so a driver that reports thousands of modes cannot make the
// host build a message the parser would reject.
inline constexpr size_t kMaxListedDisplayModes = 256;

// Modes the output reports for 32-bit, non-interlaced desktops with a refresh
// rate of 1..1000 Hz, deduplicated by size and rate. Sorted largest picture
// first, then highest refresh rate first. Returns an empty list with `error`
// set when the output is gone or reports no current mode.
std::vector<DisplayMode> list_display_modes(const DisplayInfo& display,
                                            std::string* error = nullptr);

enum class ModeRestore {
    None,      // apply() was never called
    Restored,  // the saved mode is back
    Skipped,   // the operator changed the mode outside the session
    Failed,    // ChangeDisplaySettingsExW refused; the mode is still ours
};

// One temporary mode change of one output. The change is made with
// ChangeDisplaySettingsExW flags 0, so it lasts for the desktop session and is
// never written to the registry. The original is restored only while the
// output still runs the mode this object applied; a mode the operator chose
// in the meantime is left alone.
class DisplayModeChange {
public:
    DisplayModeChange();
    ~DisplayModeChange();
    DisplayModeChange(const DisplayModeChange&) = delete;
    DisplayModeChange& operator=(const DisplayModeChange&) = delete;

    // Saves the active mode of `display`. Does nothing when a mode is already
    // saved: one session changes one output at a time, and the host restores
    // before it switches to another.
    bool save(const DisplayInfo& display, std::string* error = nullptr);
    // Applies a mode from list_display_modes() of the saved output. On
    // failure the display is left as it was and the request can be retried.
    bool apply(const DisplayMode& mode, std::string* error = nullptr);
    // Puts the saved mode back. See ModeRestore for the outcomes. restore()
    // ends this object's active state in every outcome except Failed.
    ModeRestore restore(std::string* error = nullptr);

    // True while a mode applied by this object may still be in effect.
    bool active() const;
    bool has_saved() const;
    const std::wstring& device_name() const;
    // The mode last applied successfully, in desktop space.
    DisplayMode applied_mode() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
