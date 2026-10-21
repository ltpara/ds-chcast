#include "capture/detail.h"
#include "capture/rotation.h"

#include <dxgi1_6.h>

#include <algorithm>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <cmath>

#include "chast/capture.h"

namespace chast::detail {

namespace {

struct DisplayConfigPaths {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

// The display topology can change between GetDisplayConfigBufferSizes and
// QueryDisplayConfig, which then reports ERROR_INSUFFICIENT_BUFFER instead of
// tearing. Three attempts is what the Windows samples use.
bool query_active_paths(DisplayConfigPaths& out) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
            ERROR_SUCCESS) {
            return false;
        }
        out.paths.assign(path_count, {});
        out.modes.assign(mode_count, {});
        LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, out.paths.data(),
                                         &mode_count, out.modes.data(), nullptr);
        if (result == ERROR_SUCCESS) {
            out.paths.resize(path_count);
            out.modes.resize(mode_count);
            return true;
        }
        if (result != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
    }
    return false;
}

// Walks the active paths and calls `visit` with the DISPLAYCONFIG path for the
// given GDI device name. Returning false from `visit` stops the walk.
template <typename Fn>
void for_each_path_of_device(const WCHAR* device_name, Fn&& visit) {
    DisplayConfigPaths config;
    if (!query_active_paths(config)) {
        return;
    }
    for (const auto& path : config.paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        if (std::wstring(source.viewGdiDeviceName) != device_name) {
            continue;
        }
        if (!visit(path)) {
            return;
        }
    }
}

}  // namespace

// Display order and flags from the OS. RustDesk builds its list the same way
// (EnumDisplayDevices, then reorders the DXGI outputs to match), because DXGI's
// adapter-then-output order puts a detached HDMI output before the laptop panel
// while the OS keeps the user-visible order stable.
struct GdiDisplay {
    std::wstring device_name;
    bool primary = false;
};

std::vector<GdiDisplay> enumerate_gdi_displays() {
    std::vector<GdiDisplay> displays;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) {
            break;
        }
        // DISPLAY_DEVICE_MIRRORING_DRIVER marks the mirror driver outputs of
        // virtual display drivers; they have no duplicable surface.
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0 ||
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0) {
            continue;
        }
        GdiDisplay display;
        display.device_name = device.DeviceName;
        display.primary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        displays.push_back(std::move(display));
    }
    return displays;
}

// Position of `name` in the OS order, or SIZE_MAX when the OS does not list it
// (an output DXGI sees but GDI does not, e.g. a headless virtual one).
size_t os_display_rank(const std::vector<GdiDisplay>& order, const std::wstring& name) {
    auto it = std::find_if(order.begin(), order.end(),
                           [&](const GdiDisplay& d) { return d.device_name == name; });
    return it == order.end() ? SIZE_MAX : static_cast<size_t>(it - order.begin());
}

// Returns the SDR white level for a GDI device name in DISPLAYCONFIG units
// (1000 == 80 nits == scRGB 1.0). Zero when the platform or the display does
// not report one, in which case 80 nits is assumed.
UINT query_sdr_white_level(const WCHAR* device_name) {
    if (!device_name) {
        return 0;
    }
    UINT level = 0;
    for_each_path_of_device(device_name, [&](const DISPLAYCONFIG_PATH_INFO& path) {
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
            white.SDRWhiteLevel != 0) {
            level = white.SDRWhiteLevel;
            return false;  // done
        }
        return true;
    });
    return level;
}

OutputDesc describe_output(IDXGIOutput* output) {
    OutputDesc out;
    if (!output) {
        return out;
    }
    if (FAILED(output->GetDesc(&out.desc))) {
        return out;
    }

    ComPtr<IDXGIOutput6> output6 = com_query<IDXGIOutput6>(output);
    if (!output6 || FAILED(output6->GetDesc1(&out.desc1))) {
        return out;
    }
    out.has_desc1 = true;
    // G2084 is PQ in a Rec.2020 container: only HDR desktops report it.
    // G10 (linear scRGB) is what Windows 11 22H2 and later report for
    // advanced-color SDR desktops, where 1.0 is display reference white.
    out.color.hdr = out.desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    out.color.wcg = out.desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    out.color.max_luminance = static_cast<float>(out.desc1.MaxLuminance);
    if (out.color.hdr) {
        out.color.sdr_white_level = query_sdr_white_level(out.desc.DeviceName);
    }
    return out;
}

bool query_color_state(const std::wstring& device_name, ColorState& out) {
    if (device_name.empty()) {
        return false;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.put())))) {
        return false;
    }
    // The factory we were handed when the capturer was created may be stale:
    // a DXGI output description is a snapshot, and EnumOutputs on a stale
    // factory keeps returning the old color space forever. Enumerating from a
    // fresh factory is what the DXGI docs require for a current view.
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, adapter.put()) == DXGI_ERROR_NOT_FOUND) {
            return false;
        }
        if (!adapter) {
            return false;
        }
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, output.put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!output) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || std::wstring(desc.DeviceName) != device_name) {
                continue;
            }
            OutputDesc described = describe_output(output.get());
            if (!described.has_desc1) {
                return false;
            }
            out = described.color;
            return true;
        }
    }
    return false;
}

DuplicationDesc describe_duplication(IDXGIOutputDuplication* duplication, IDXGIOutput* output) {
    DuplicationDesc out;
    if (duplication) {
        // GetDesc on the duplication is void: it cannot fail.
        duplication->GetDesc(&out.desc);
    }
    if (output) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc))) {
            out.rotation = desc.Rotation;
        }
        out.color = describe_output(output).color;
    }
    return out;
}

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(len, 0)), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(),
                            len, nullptr, nullptr);
    }
    return out;
}

ColorStateMonitor::ColorStateMonitor(std::wstring device_name)
    : device_name_(std::move(device_name)) {}

ColorState ColorStateMonitor::refresh(bool force, bool* changed) {
    if (changed) {
        *changed = false;
    }
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kInterval = std::chrono::seconds(1);
    if (!force && initialized_ && now - last_query_ < kInterval) {
        return state_;
    }
    last_query_ = now;

    ColorState fresh;
    if (!query_color_state(device_name_, fresh)) {
        // Keep the last known state; a transient failure (topology in flux)
        // should not flip the whole pipeline from HDR to SDR.
        return state_;
    }
    if (initialized_ && (fresh.hdr != state_.hdr || fresh.wcg != state_.wcg ||
                         fresh.sdr_white_level != state_.sdr_white_level)) {
        if (changed) {
            *changed = true;
        }
    }
    state_ = fresh;
    initialized_ = true;
    return state_;
}

}  // namespace chast::detail

namespace chast {

// Public API (chast:: namespace): the capturer and the tools call this.
std::vector<DisplayInfo> enumerate_displays() {
    using detail::describe_output;
    using detail::os_display_rank;
    using detail::OutputDesc;
    using detail::rotation_degrees;
    std::vector<DisplayInfo> displays;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.put())))) {
        return displays;
    }
    const std::vector<detail::GdiDisplay> os_order = detail::enumerate_gdi_displays();

    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, adapter.put()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) {
            break;
        }

        DXGI_ADAPTER_DESC1 adapter_desc{};
        const bool have_adapter_desc = SUCCEEDED(adapter->GetDesc1(&adapter_desc));

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, output.put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!output) {
                break;
            }

            OutputDesc described = describe_output(output.get());
            const LONG width = described.desc.DesktopCoordinates.right -
                               described.desc.DesktopCoordinates.left;
            const LONG height = described.desc.DesktopCoordinates.bottom -
                                described.desc.DesktopCoordinates.top;
            // Win+P "show only on 1" still leaves the other outputs attached
            // with a zero-sized rect. Duplicating one of those acquires frames
            // of a dead surface, and a zero-sized frame would make the shader
            // stages build an empty viewport, so they are not displays at all.
            if (!described.desc.AttachedToDesktop || width <= 0 || height <= 0) {
                continue;
            }

            DisplayInfo info;
            info.device_name = described.desc.DeviceName;
            if (have_adapter_desc) {
                info.adapter_name = adapter_desc.Description;
                info.adapter_luid = adapter_desc.AdapterLuid;
                info.adapter_vendor_id = adapter_desc.VendorId;
            }
            info.rect.left = described.desc.DesktopCoordinates.left;
            info.rect.top = described.desc.DesktopCoordinates.top;
            info.rect.right = described.desc.DesktopCoordinates.right;
            info.rect.bottom = described.desc.DesktopCoordinates.bottom;
            // The OS knows which output is primary. The virtual-desktop origin
            // does not imply it: a display to the left of the primary has a
            // negative origin, and a detached output can report 0,0.
            auto listed = std::find_if(os_order.begin(), os_order.end(), [&](const auto& d) {
                return d.device_name == info.device_name;
            });
            info.primary = listed != os_order.end() && listed->primary;
            info.rotation_degrees = rotation_degrees(described.desc.Rotation);
            // DesktopCoordinates are already in virtual-desktop space, i.e.
            // the orientation the user sees: a portrait panel rotated 90
            // reports a portrait rect here while the duplicated surface stays
            // landscape. That makes these the dimensions of a captured frame
            // after rotation, which is what DisplayInfo documents.
            info.width = static_cast<UINT>(width);
            info.height = static_cast<UINT>(height);
            info.monitor = described.desc.Monitor;
            info.hdr = described.color.hdr;
            info.advanced_color_sdr = described.color.wcg;
            info.sdr_white_level = described.color.sdr_white_level;
            info.max_luminance_nits = described.color.max_luminance;
            info.adapter_index = adapter_index;
            info.output_index = output_index;
            displays.push_back(std::move(info));
        }
    }

    // RustDesk orders its list the way the OS lists displays, so a display
    // keeps its index across a topology change that DXGI reports in a
    // different adapter order. Outputs the OS does not list (headless virtual
    // adapters) follow, primary first, then by virtual-desktop position.
    std::stable_sort(displays.begin(), displays.end(),
                     [&](const DisplayInfo& a, const DisplayInfo& b) {
                         const size_t rank_a = os_display_rank(os_order, a.device_name);
                         const size_t rank_b = os_display_rank(os_order, b.device_name);
                         if ((rank_a == SIZE_MAX) != (rank_b == SIZE_MAX)) {
                             return rank_b == SIZE_MAX;
                         }
                         if (rank_a != rank_b) {
                             return rank_a < rank_b;
                         }
                         if (a.primary != b.primary) {
                             return a.primary;
                         }
                         if (a.rect.left != b.rect.left) {
                             return a.rect.left < b.rect.left;
                         }
                         return a.rect.top < b.rect.top;
                     });
    return displays;
}

std::string display_name_utf8(const DisplayInfo& display) {
    if (display.device_name.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, display.device_name.data(),
                                         static_cast<int>(display.device_name.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string name(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, display.device_name.data(),
                        static_cast<int>(display.device_name.size()), name.data(), size,
                        nullptr, nullptr);
    return name;
}

}  // namespace chast
