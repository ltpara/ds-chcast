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
// injector. Like a shared whiteboard it tells viewers apart: the arrow and
// its label take a colour derived from the viewer's device name, a click
// sends a ripple, and the name is drawn next to the arrow.

#include <cstdint>
#include <memory>
#include <string>

namespace chast {

class CursorHintOverlay {
public:
    // Starts the window thread. `label` is the viewer's device name, drawn
    // next to the arrow and hashed into its colour; an empty name keeps the
    // plain white arrow. Returns nullptr with *error set when the overlay
    // window cannot be created (for example on a non-interactive desktop).
    static std::unique_ptr<CursorHintOverlay> create(std::string* error,
                                                     const std::string& label = {});

    ~CursorHintOverlay();
    CursorHintOverlay(const CursorHintOverlay&) = delete;
    CursorHintOverlay& operator=(const CursorHintOverlay&) = delete;

    // Moves the drawn pointer to (x, y) and shows it. `pressed` is the
    // viewer's click edge: it starts the ripple, it is not a held-button
    // state. Returns false with *error set when the window or the placement
    // failed.
    bool move(int32_t x, int32_t y, bool pressed, std::string* error);

    // Hides the pointer. Safe to call when it is already hidden.
    void hide();

    bool visible() const;

private:
    CursorHintOverlay();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
