#pragma once

// Removes the interactive desktop's wallpaper for the duration of a session
// and restores it afterwards (RustDesk's allow-remove-wallpaper). A flat
// background costs far less to encode than a photo, so this lowers the cost of
// static desktop frames without touching the zero-copy capture path.
//
// The original path is written to `state_path` before the wallpaper is
// removed. A host that is killed mid-session leaves that backup behind; the
// next start calls restore_abandoned_wallpaper() to put the wallpaper back.
// The state file is only written by the removal that actually changed the
// desktop, so a second session that sees an already solid color is a no-op.

#include <memory>
#include <string>

namespace chast {

class WallpaperRemoval {
public:
    virtual ~WallpaperRemoval() = default;

    // Put the saved wallpaper back and delete the backup. Idempotent; returns
    // false with a diagnostic when the desktop could not be restored (the
    // backup is kept in that case).
    virtual bool restore(std::string* error) = 0;
    // True while the saved wallpaper still needs restoring. A no-op removal
    // (the desktop was already solid) reports false.
    virtual bool active() const = 0;
};

// Saves the current wallpaper to `state_path` and clears it. Returns nullptr
// with `error` when there is no wallpaper to remove or the desktop cannot be
// changed; `state_path` must be non-empty.
std::unique_ptr<WallpaperRemoval> create_wallpaper_removal(const std::string& state_path,
                                                           std::string* error);

// Restores a wallpaper left removed by a host that exited without restoring.
// Returns true when there was nothing to do or the restore succeeded.
bool restore_abandoned_wallpaper(const std::string& state_path, std::string* error);

}  // namespace chast
