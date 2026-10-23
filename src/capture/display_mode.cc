#define NOMINMAX
#include "chast/display_mode.h"

#include <algorithm>
#include <string>

namespace chast {

namespace {

bool swaps_axes(UINT rotation) {
    return rotation == 90 || rotation == 270;
}

std::string win32_error(const char* call) {
    return std::string(call) + " failed with error " + std::to_string(GetLastError());
}

const char* change_result_name(LONG result) {
    switch (result) {
        case DISP_CHANGE_SUCCESSFUL:
            return "DISP_CHANGE_SUCCESSFUL";
        case DISP_CHANGE_RESTART:
            return "DISP_CHANGE_RESTART";
        case DISP_CHANGE_FAILED:
            return "DISP_CHANGE_FAILED";
        case DISP_CHANGE_BADMODE:
            return "DISP_CHANGE_BADMODE";
        case DISP_CHANGE_NOTUPDATED:
            return "DISP_CHANGE_NOTUPDATED";
        case DISP_CHANGE_BADFLAGS:
            return "DISP_CHANGE_BADFLAGS";
        case DISP_CHANGE_BADPARAM:
            return "DISP_CHANGE_BADPARAM";
        case DISP_CHANGE_BADDUALVIEW:
            return "DISP_CHANGE_BADDUALVIEW";
        default:
            return "an unknown result";
    }
}

bool query_current(const std::wstring& device_name, DEVMODEW& mode, std::string* error) {
    mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
        if (error) {
            *error = win32_error("EnumDisplaySettingsExW");
        }
        return false;
    }
    return true;
}

bool change_mode(const std::wstring& device_name, DEVMODEW mode, std::string* error) {
    const LONG result = ChangeDisplaySettingsExW(device_name.c_str(), &mode, nullptr, 0, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        if (error) {
            *error = std::string("ChangeDisplaySettingsExW returned ") + change_result_name(result);
        }
        return false;
    }
    return true;
}

DisplayMode desktop_mode(const DEVMODEW& mode, bool swap) {
    DisplayMode out;
    out.width = static_cast<uint16_t>(swap ? mode.dmPelsHeight : mode.dmPelsWidth);
    out.height = static_cast<uint16_t>(swap ? mode.dmPelsWidth : mode.dmPelsHeight);
    out.refresh_hz = static_cast<uint16_t>(mode.dmDisplayFrequency);
    return out;
}

}  // namespace

struct DisplayModeChange::Impl {
    bool saved = false;
    std::wstring device_name;
    UINT rotation = 0;
    DEVMODEW original{};
    bool applied = false;
    DisplayMode applied_mode;
    // The mode the driver actually runs, in native orientation.
    UINT applied_width = 0;
    UINT applied_height = 0;
    UINT applied_refresh = 0;
};

DisplayModeChange::DisplayModeChange() : impl_(std::make_unique<Impl>()) {}
DisplayModeChange::~DisplayModeChange() = default;

std::vector<DisplayMode> list_display_modes(const DisplayInfo& display, std::string* error) {
    std::vector<DisplayMode> modes;
    if (display.device_name.empty()) {
        if (error) {
            *error = "the display has no device name";
        }
        return modes;
    }
    DEVMODEW current{};
    if (!query_current(display.device_name, current, error)) {
        return modes;
    }
    const bool swap = swaps_axes(display.rotation_degrees);
    const UINT current_width = swap ? current.dmPelsHeight : current.dmPelsWidth;
    const UINT current_height = swap ? current.dmPelsWidth : current.dmPelsHeight;
    const UINT current_refresh = current.dmDisplayFrequency;
    bool current_marked = false;

    for (DWORD index = 0; modes.size() < kMaxListedDisplayModes; ++index) {
        DEVMODEW native{};
        native.dmSize = sizeof(native);
        if (!EnumDisplaySettingsExW(display.device_name.c_str(), index, &native, 0)) {
            break;
        }
        if (native.dmBitsPerPel != 32 || (native.dmDisplayFlags & DM_INTERLACED) != 0) {
            continue;
        }
        if (native.dmPelsWidth == 0 || native.dmPelsHeight == 0 || native.dmPelsWidth > 16384 ||
            native.dmPelsHeight > 16384) {
            continue;
        }
        if (native.dmDisplayFrequency < 1 || native.dmDisplayFrequency > 1000) {
            continue;
        }
        DisplayMode mode = desktop_mode(native, swap);
        const bool duplicate = std::any_of(modes.begin(), modes.end(), [&](const DisplayMode& m) {
            return m.width == mode.width && m.height == mode.height &&
                   m.refresh_hz == mode.refresh_hz;
        });
        if (duplicate) {
            continue;
        }
        // A driver that reports 0 Hz as the active rate does not identify one
        // of its own modes; the fastest entry of that size is the one it runs.
        if (!current_marked && mode.width == current_width && mode.height == current_height &&
            (current_refresh == 0 || mode.refresh_hz == current_refresh)) {
            mode.current = true;
            current_marked = true;
        }
        modes.push_back(mode);
    }

    std::sort(modes.begin(), modes.end(), [](const DisplayMode& a, const DisplayMode& b) {
        const uint32_t area_a = static_cast<uint32_t>(a.width) * a.height;
        const uint32_t area_b = static_cast<uint32_t>(b.width) * b.height;
        if (area_a != area_b) {
            return area_a > area_b;
        }
        if (a.width != b.width) {
            return a.width > b.width;
        }
        if (a.refresh_hz != b.refresh_hz) {
            return a.refresh_hz > b.refresh_hz;
        }
        return a.height > b.height;
    });
    if (modes.empty() && error) {
        *error = "the display reports no usable modes";
    }
    return modes;
}

bool DisplayModeChange::save(const DisplayInfo& display, std::string* error) {
    if (impl_->saved) {
        return true;
    }
    if (display.device_name.empty()) {
        if (error) {
            *error = "the display has no device name";
        }
        return false;
    }
    DEVMODEW current{};
    if (!query_current(display.device_name, current, error)) {
        return false;
    }
    impl_->saved = true;
    impl_->device_name = display.device_name;
    impl_->rotation = display.rotation_degrees;
    impl_->original = current;
    return true;
}

bool DisplayModeChange::apply(const DisplayMode& mode, std::string* error) {
    if (!impl_->saved) {
        if (error) {
            *error = "no display mode was saved";
        }
        return false;
    }
    DEVMODEW current{};
    if (!query_current(impl_->device_name, current, error)) {
        return false;
    }
    const bool swap = swaps_axes(impl_->rotation);
    DEVMODEW next = current;
    next.dmPelsWidth = swap ? mode.height : mode.width;
    next.dmPelsHeight = swap ? mode.width : mode.height;
    next.dmDisplayFrequency = mode.refresh_hz;
    next.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    if (!change_mode(impl_->device_name, next, error)) {
        return false;
    }

    // The driver may land on a nearby mode; the applied mode is what the
    // output runs, which is what restore() later compares against.
    DEVMODEW actual{};
    if (!query_current(impl_->device_name, actual, nullptr)) {
        actual = next;
    }
    impl_->applied = true;
    impl_->applied_width = actual.dmPelsWidth;
    impl_->applied_height = actual.dmPelsHeight;
    impl_->applied_refresh = actual.dmDisplayFrequency;
    impl_->applied_mode = desktop_mode(actual, swap);
    impl_->applied_mode.current = true;
    return true;
}

ModeRestore DisplayModeChange::restore(std::string* error) {
    if (!impl_->applied) {
        return ModeRestore::None;
    }
    DEVMODEW current{};
    if (!query_current(impl_->device_name, current, error)) {
        return ModeRestore::Failed;
    }
    if (current.dmPelsWidth != impl_->applied_width ||
        current.dmPelsHeight != impl_->applied_height ||
        current.dmDisplayFrequency != impl_->applied_refresh) {
        // Someone else chose a mode; putting ours back would fight them.
        impl_->applied = false;
        return ModeRestore::Skipped;
    }
    DEVMODEW next = current;
    next.dmPelsWidth = impl_->original.dmPelsWidth;
    next.dmPelsHeight = impl_->original.dmPelsHeight;
    next.dmDisplayFrequency = impl_->original.dmDisplayFrequency;
    next.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    if (!change_mode(impl_->device_name, next, error)) {
        return ModeRestore::Failed;
    }
    impl_->applied = false;
    return ModeRestore::Restored;
}

bool DisplayModeChange::active() const {
    return impl_->applied;
}

bool DisplayModeChange::has_saved() const {
    return impl_->saved;
}

const std::wstring& DisplayModeChange::device_name() const {
    return impl_->device_name;
}

DisplayMode DisplayModeChange::applied_mode() const {
    return impl_->applied_mode;
}

}  // namespace chast
