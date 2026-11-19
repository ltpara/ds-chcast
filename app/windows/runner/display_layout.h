#ifndef RUNNER_DISPLAY_LAYOUT_H_
#define RUNNER_DISPLAY_LAYOUT_H_

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chast {

// Separate display windows remember their physical position and size per
// device and host display index, the way RustDesk reopens remote displays
// where the operator left them. Values are physical pixels, so they are
// applied without another DPI scaling pass.
inline constexpr uint32_t kMaxDisplayLayoutEntries = 256;
inline constexpr int kMinDisplayWindowWidth = 320;
inline constexpr int kMinDisplayWindowHeight = 240;

// Maps a remote output into the local virtual desktop while preserving the
// remote topology's scale and relative placement.
inline std::optional<RECT> MapRemoteDisplayBounds(const RECT& remote_display,
                                                 const RECT& remote_desktop,
                                                 const RECT& local_desktop) {
  const long long remote_width = static_cast<long long>(remote_desktop.right) -
                                 remote_desktop.left;
  const long long remote_height = static_cast<long long>(remote_desktop.bottom) -
                                  remote_desktop.top;
  const long long local_width = static_cast<long long>(local_desktop.right) -
                                local_desktop.left;
  const long long local_height = static_cast<long long>(local_desktop.bottom) -
                                 local_desktop.top;
  if (remote_width <= 0 || remote_height <= 0 || local_width <= 0 ||
      local_height <= 0 || remote_display.left < remote_desktop.left ||
      remote_display.top < remote_desktop.top ||
      remote_display.right > remote_desktop.right ||
      remote_display.bottom > remote_desktop.bottom ||
      remote_display.right <= remote_display.left ||
      remote_display.bottom <= remote_display.top) {
    return std::nullopt;
  }

  const double scale = std::min(
      static_cast<double>(local_width) / remote_width,
      static_cast<double>(local_height) / remote_height);
  const double drawn_width = remote_width * scale;
  const double drawn_height = remote_height * scale;
  const double offset_x =
      local_desktop.left + (local_width - drawn_width) / 2.0;
  const double offset_y =
      local_desktop.top + (local_height - drawn_height) / 2.0;
  const auto map_x = [&](LONG x) {
    return static_cast<LONG>(std::llround(
        offset_x + (static_cast<double>(x) - remote_desktop.left) * scale));
  };
  const auto map_y = [&](LONG y) {
    return static_cast<LONG>(std::llround(
        offset_y + (static_cast<double>(y) - remote_desktop.top) * scale));
  };
  RECT mapped{map_x(remote_display.left), map_y(remote_display.top),
              map_x(remote_display.right), map_y(remote_display.bottom)};
  if (mapped.right <= mapped.left || mapped.bottom <= mapped.top) {
    return std::nullopt;
  }
  return mapped;
}

inline bool HasUsableDisplayIntersection(const RECT& bounds,
                                         const std::vector<RECT>& monitors) {
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return false;
  for (const RECT& monitor : monitors) {
    const long long width =
        static_cast<long long>(std::min(bounds.right, monitor.right)) -
        std::max(bounds.left, monitor.left);
    const long long height =
        static_cast<long long>(std::min(bounds.bottom, monitor.bottom)) -
        std::max(bounds.top, monitor.top);
    if (width >= kMinDisplayWindowWidth &&
        height >= kMinDisplayWindowHeight) {
      return true;
    }
  }
  return false;
}

// %APPDATA%\chast\display-windows.tsv. An unavailable roaming profile yields
// an empty path, which load/save treat as "no preference".
std::filesystem::path DisplayLayoutPath();

// Chooses the primary local monitor for one remote display, or maps a
// multi-display topology onto the local virtual desktop.
std::optional<RECT> AutoDisplayBounds(const RECT& remote_display,
                                      const RECT& remote_desktop,
                                      uint32_t remote_display_count);
bool DisplayBoundsHaveUsableVisibility(const RECT& bounds);

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
