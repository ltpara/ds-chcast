#ifndef RUNNER_DISPLAY_LAYOUT_H_
#define RUNNER_DISPLAY_LAYOUT_H_

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace chast {

// Separate display windows remember their physical position and size per
// device and host display index, the way RustDesk reopens remote displays
// where the operator left them. Values are physical pixels, so they are
// applied without another DPI scaling pass.
inline constexpr uint32_t kMaxDisplayLayoutEntries = 256;
inline constexpr int kMinDisplayWindowWidth = 320;
inline constexpr int kMinDisplayWindowHeight = 240;

// %APPDATA%\chast\display-windows.tsv. An unavailable roaming profile yields
// an empty path, which load/save treat as "no preference".
std::filesystem::path DisplayLayoutPath();

// A saved rectangle is ignored when it no longer intersects any monitor, so
// unplugging a screen does not strand the next window offscreen.
std::optional<RECT> LoadDisplayBounds(const std::filesystem::path& file,
                                      const std::string& peer_id,
                                      uint32_t display_index);
void SaveDisplayBounds(const std::filesystem::path& file,
                       const std::string& peer_id, uint32_t display_index,
                       const RECT& bounds);
bool DisplayBoundsVisible(const RECT& bounds);

}  // namespace chast

#endif  // RUNNER_DISPLAY_LAYOUT_H_
