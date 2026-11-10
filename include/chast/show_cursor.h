#pragma once

// "Show my cursor": draws the viewer's pointer on the host desktop so the
// person in front of the machine sees where the remote viewer is pointing.
//
// The drawing lives in a layered, click-through, never-activated tool window
// on its own thread, so it cannot take focus, intercept input or appear in
// Alt+Tab. Coordinates are virtual-desktop pixels, the same space InputEvent
// uses; the overlay clamps them so a stale hint cannot place the pointer
// outside every monitor.
//
// The overlay is a hint only: no coordinate it receives ever reaches the
// injector.

#include <cstdint>
#include <memory>
#include <string>

namespace chast {

class CursorHintOverlay {
public:
    // Starts the window thread. Returns nullptr with *error set when the
    // overlay window cannot be created (for example on a non-interactive
    // desktop).
    static std::unique_ptr<CursorHintOverlay> create(std::string* error);

    ~CursorHintOverlay();
    CursorHintOverlay(const CursorHintOverlay&) = delete;
    CursorHintOverlay& operator=(const CursorHintOverlay&) = delete;

    // Moves the drawn pointer to (x, y) and shows it. Returns false with
    // *error set when the window or the placement failed.
    bool move(int32_t x, int32_t y, std::string* error);

    // Hides the pointer. Safe to call when it is already hidden.
    void hide();

    bool visible() const;

private:
    CursorHintOverlay();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
