#include "display_topology.h"
#include "chast/display_privacy.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>

namespace chast::display_topology {
namespace {
bool same_luid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
}

bool query(Snapshot& snapshot, UINT flags, std::string* error) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        UINT paths = 0, modes = 0;
        LONG code = GetDisplayConfigBufferSizes(flags, &paths, &modes);
        if (code == ERROR_SUCCESS && paths <= 4096 && modes <= 8192) {
            snapshot.paths.resize(paths);
            snapshot.modes.resize(modes);
            code = QueryDisplayConfig(flags, &paths, snapshot.paths.data(), &modes,
                                      snapshot.modes.data(), nullptr);
            if (code == ERROR_INSUFFICIENT_BUFFER) continue;
            if (code == ERROR_SUCCESS) {
                snapshot.paths.resize(paths);
                snapshot.modes.resize(modes);
                return true;
            }
        }
        if (error) *error = "cannot read display topology (" + std::to_string(code) + ")";
        return false;
    }
    if (error) *error = "display topology is changing; retry when it is stable";
    return false;
}

bool valid(const Snapshot& snapshot) {
    if (snapshot.paths.empty() || snapshot.paths.size() > 64 || snapshot.modes.size() > 256)
        return false;
    for (const auto& path : snapshot.paths) {
        const UINT source = path.sourceInfo.modeInfoIdx;
        const UINT target = path.targetInfo.modeInfoIdx;
        if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE) || source >= snapshot.modes.size() ||
            target >= snapshot.modes.size()) return false;
        const auto& s = snapshot.modes[source];
        const auto& t = snapshot.modes[target];
        if (s.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
            t.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET ||
            s.id != path.sourceInfo.id || t.id != path.targetInfo.id ||
            !same_luid(s.adapterId, path.sourceInfo.adapterId) ||
            !same_luid(t.adapterId, path.targetInfo.adapterId) ||
            !s.sourceMode.width || !s.sourceMode.height) return false;
    }
    return true;
}

bool isolate(const Snapshot& before, size_t index, const VirtualDisplayMode& mode,
             Snapshot& after, std::string* error) {
    if (!valid(before) || index >= before.paths.size() || mode.width < 640 ||
        mode.width > 7680 || mode.height < 480 || mode.height > 4320 ||
        !mode.refresh_hz || mode.refresh_hz > 1000) {
        if (error) *error = "invalid virtual display topology or mode";
        return false;
    }
    auto path = before.paths[index];
    auto source = before.modes[path.sourceInfo.modeInfoIdx];
    source.sourceMode.position = {0, 0};
    source.sourceMode.width = mode.width;
    source.sourceMode.height = mode.height;
    path.sourceInfo.modeInfoIdx = 0;
    // Windows chooses the target signal timing for the requested source mode.
    path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    path.targetInfo.refreshRate = {mode.refresh_hz, 1};
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    after.paths = {path};
    after.modes = {source};
    return true;
}
}  // namespace chast::display_topology

namespace chast {
namespace {
using display_topology::Snapshot;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    void reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct RecoveryData {
    uint32_t magic = 0x43564431;  // CVD1; only accepted from an inherited section.
    UINT paths = 0;
    UINT modes = 0;
    DISPLAYCONFIG_PATH_INFO path[64]{};
    DISPLAYCONFIG_MODE_INFO mode[256]{};
};

bool fail(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

LONG apply(Snapshot snapshot, bool allow_changes) {
    return SetDisplayConfig(static_cast<UINT>(snapshot.paths.size()), snapshot.paths.data(),
                            static_cast<UINT>(snapshot.modes.size()), snapshot.modes.data(),
                            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                (allow_changes ? SDC_ALLOW_CHANGES : 0));
}

bool same_target(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
    return a.targetInfo.id == b.targetInfo.id &&
           a.targetInfo.adapterId.LowPart == b.targetInfo.adapterId.LowPart &&
           a.targetInfo.adapterId.HighPart == b.targetInfo.adapterId.HighPart;
}

std::wstring monitor_path(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = path.targetInfo.adapterId;
    name.header.id = path.targetInfo.id;
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS
               ? name.monitorDevicePath : L"";
}

// Restore exact modes/positions first. If a monitor was unplugged, restore
// the surviving original outputs; keep the guardian alive when none remain.
// Never ask Windows for an arbitrary topology that could keep all panels off.
bool restore_snapshot(const Snapshot& saved) {
    const auto restored = [](const Snapshot& wanted) {
        Snapshot actual;
        std::string ignored;
        if (!display_topology::query(actual, QDC_ONLY_ACTIVE_PATHS, &ignored) ||
            actual.paths.size() != wanted.paths.size()) return false;
        return std::all_of(wanted.paths.begin(), wanted.paths.end(), [&](const auto& old) {
            return std::any_of(actual.paths.begin(), actual.paths.end(), [&](const auto& current) {
                return same_target(old, current);
            });
        });
    };
    if ((apply(saved, false) == ERROR_SUCCESS || apply(saved, true) == ERROR_SUCCESS) &&
        restored(saved)) return true;
    Snapshot all;
    std::string ignored;
    if (!display_topology::query(all, QDC_ALL_PATHS, &ignored)) return false;
    Snapshot available = saved;
    std::erase_if(available.paths, [&](const auto& old) {
        return std::none_of(all.paths.begin(), all.paths.end(), [&](const auto& current) {
            return current.targetInfo.targetAvailable && same_target(old, current);
        });
    });
    if (available.paths.empty()) return false;
    // If the old primary is gone, move a surviving source to the origin.
    bool primary = false;
    for (const auto& path : available.paths) {
        const auto& mode = available.modes[path.sourceInfo.modeInfoIdx].sourceMode;
        primary = primary || (mode.position.x == 0 && mode.position.y == 0);
    }
    if (!primary) {
        const auto origin = available.modes[available.paths[0].sourceInfo.modeInfoIdx]
                                .sourceMode.position;
        for (auto& mode : available.modes) {
            if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                mode.sourceMode.position.x -= origin.x;
                mode.sourceMode.position.y -= origin.y;
            }
        }
    }
    return apply(available, true) == ERROR_SUCCESS && restored(available);
}

class WindowsDisplayPrivacy final : public DisplayPrivacyBackend {
public:
    WindowsDisplayPrivacy(std::string resolution, std::wstring package, bool management = false)
        : resolution_(std::move(resolution)), package_(std::move(package)), management_(management) {}
    ~WindowsDisplayPrivacy() override {
        // A timed-out restore is left to the independent guardian. Its event,
        // snapshot and desktop lease survive closing these parent handles.
        if (restore_.value) SetEvent(restore_.value);
    }

    bool arm(std::string* error) override {
        if (guard_.value) return fail(error, "display recovery is already armed");
        // Take the lease before reading the topology, so another process
        // cannot finish a topology change between the snapshot and ownership.
        Handle lease;
        lease.reset(CreateMutexW(nullptr, FALSE, L"Local\\chast-display-privacy-v1"));
        const DWORD lease_error = GetLastError();
        if (!lease.value || lease_error == ERROR_ALREADY_EXISTS)
            return fail(error, "another virtual display privacy operation is active or recovering");
        const auto status = classify_virtual_display_probe(probe_virtual_display());
        if (status.state != VirtualDisplayState::Active &&
            status.state != VirtualDisplayState::Installed) return fail(error, status.detail);
        Snapshot snapshot;
        if (!display_topology::query(snapshot, QDC_ONLY_ACTIVE_PATHS, error) ||
            !display_topology::valid(snapshot)) return fail(error, "cannot save the active display topology");
        bool physical = false;
        for (const auto& path : snapshot.paths) {
            const auto monitor = monitor_path(path);
            if (monitor.empty()) return fail(error, "cannot identify an active display");
            physical = physical || !is_usbmmidd_monitor(monitor);
        }
        if (!physical && !management_) return fail(error, "there are no physical displays to hide");
        saved_ = snapshot;

        // Existence is the lease: the guardian retains it even after its owner
        // crashes. No second host can take another snapshot of a private desktop.
        Handle section, ready, parent;
        section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(RecoveryData), nullptr));
        ready.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        restore_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        parent.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 GetCurrentProcessId()));
        if (!section.value || !ready.value || !restore_.value || !parent.value)
            return fail(error, "cannot allocate display recovery handles");
        auto* data = static_cast<RecoveryData*>(MapViewOfFile(section.value, FILE_MAP_WRITE,
                                                             0, 0, sizeof(RecoveryData)));
        if (!data) return fail(error, "cannot map display recovery snapshot");
        *data = RecoveryData{};
        data->paths = static_cast<UINT>(snapshot.paths.size());
        data->modes = static_cast<UINT>(snapshot.modes.size());
        std::copy(snapshot.paths.begin(), snapshot.paths.end(), data->path);
        std::copy(snapshot.modes.begin(), snapshot.modes.end(), data->mode);
        UnmapViewOfFile(data);

        const std::array<HANDLE, 5> originals = {parent.value, section.value, ready.value,
                                               restore_.value, lease.value};
        const std::array<DWORD, 5> rights = {SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FILE_MAP_READ, EVENT_MODIFY_STATE, SYNCHRONIZE, SYNCHRONIZE};
        std::array<Handle, 5> inherited;
        std::array<HANDLE, 5> handles{};
        for (size_t i = 0; i < handles.size(); ++i) {
            if (!DuplicateHandle(GetCurrentProcess(), originals[i], GetCurrentProcess(),
                                 &inherited[i].value, rights[i], TRUE, 0))
                return fail(error, "cannot duplicate display recovery handles");
            handles[i] = inherited[i].value;
        }
        wchar_t exe[32768]{};
        const DWORD size = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (!size || size == std::size(exe)) return fail(error, "cannot locate recovery helper");
        const auto helper = (std::filesystem::path(exe).parent_path() /
                             L"chast-display-guard.exe").wstring();
        std::wstring command = L"\"" + helper + L"\"";
        for (HANDLE h : handles) command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(h));
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<BYTE> storage(bytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
            return fail(error, "cannot create recovery process attributes");
        const bool attributes = UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof(handles), nullptr, nullptr) != FALSE;
        BOOL in_job = FALSE;
        IsProcessInJob(GetCurrentProcess(), nullptr, &in_job);
        PROCESS_INFORMATION child{};
        const DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                            (in_job ? CREATE_BREAKAWAY_FROM_JOB : 0);
        const bool started = attributes && CreateProcessW(helper.c_str(), command.data(), nullptr,
            nullptr, TRUE, flags, nullptr, nullptr, &startup.StartupInfo, &child);
        const DWORD create_error = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!started) return fail(error, "cannot start independent display recovery helper (" +
                                  std::to_string(create_error) + ")");
        CloseHandle(child.hThread);
        guard_.reset(child.hProcess);
        HANDLE waits[] = {ready.value, guard_.value};
        if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0) {
            SetEvent(restore_.value);
            guard_.reset();
            return fail(error, "display recovery helper did not acknowledge the snapshot");
        }
        return true;
    }

    bool configure(uint32_t count, std::vector<std::wstring>* monitors, std::string* error) override {
        if (!management_ || !guard_alive() || count > kMaxVirtualDisplayMonitors)
            return fail(error, "display management is not armed or the count is invalid");
        // Never deactivate a pre-existing virtual output: the driver cannot
        // target deletion, and those outputs may be the only headless desktop.
        uint32_t baseline = 0;
        for (const auto& path : saved_.paths) if (is_usbmmidd_monitor(monitor_path(path))) ++baseline;
        if (count < baseline) return fail(error, "cannot remove virtual displays that predate this session");
        auto provider = make_virtual_display_provider(VirtualDisplayProviderKind::Usbmmidd, package_);
        const auto status = provider->status();
        VirtualDisplayInstallRecord record;
        if (!read_install_record(&record) || !record.owned)
            return fail(error, "remote display management requires a driver installed through chast");
        if (status.active_monitors < count && !provider->set_monitor_count(count, error)) return false;
        if (count && !attach_virtual_display_outputs(count, error)) return false;
        Snapshot current;
        if (!display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error)) return false;
        uint32_t retained = baseline;
        std::erase_if(current.paths, [&](const auto& path) {
            if (!is_usbmmidd_monitor(monitor_path(path))) return false;
            if (std::any_of(saved_.paths.begin(), saved_.paths.end(), [&](const auto& p) { return same_target(p, path); }))
                return false;
            return retained++ >= count;
        });
        if (current.paths.empty() || !display_topology::valid(current))
            return fail(error, "the requested layout would leave no usable desktop");
        if (apply(current, true) != ERROR_SUCCESS) return fail(error, "cannot apply the virtual display layout");
        if (!display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error)) return false;
        monitors->clear();
        for (const auto& path : current.paths) {
            const auto monitor = monitor_path(path);
            if (is_usbmmidd_monitor(monitor)) monitors->push_back(monitor);
        }
        if (monitors->size() != count) return fail(error, "Windows did not apply the requested virtual display count");
        return intact(error);
    }

    bool toggle(const std::wstring& monitor, bool enabled,
                std::vector<std::wstring>* monitors, std::string* error) override {
        if (!management_ || !guard_alive()) return fail(error, "display management is not armed");
        VirtualDisplayInstallRecord record;
        if (!read_install_record(&record) || !record.owned)
            return fail(error, "remote display management requires a driver installed through chast");
        const auto probe = probe_virtual_display();
        if (!probe.scan_error.empty() || !probe.conflicting_driver.empty())
            return fail(error, "virtual display scan failed or another driver is present");
        Snapshot current, all;
        if (!display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error) ||
            !display_topology::query(all, QDC_ALL_PATHS, error)) return false;
        const auto matches = [&](const auto& path) {
            return path.targetInfo.targetAvailable && _wcsicmp(monitor_path(path).c_str(), monitor.c_str()) == 0;
        };
        if (!is_usbmmidd_monitor(monitor) || !std::any_of(all.paths.begin(), all.paths.end(), matches))
            return fail(error, "the selected virtual output is no longer available");
        const bool active = std::any_of(current.paths.begin(), current.paths.end(), matches);
        if (active != enabled) {
            if (!enabled) {
                std::erase_if(current.paths, matches);
                if (current.paths.empty()) return fail(error, "cannot disable the last active desktop output");
                // If this output was primary, place a surviving source at the origin.
                bool primary = false;
                for (const auto& path : current.paths) {
                    const auto& mode = current.modes[path.sourceInfo.modeInfoIdx].sourceMode;
                    primary = primary || (mode.position.x == 0 && mode.position.y == 0);
                }
                if (!primary) {
                    const auto origin = current.modes[current.paths.front().sourceInfo.modeInfoIdx].sourceMode.position;
                    for (auto& mode : current.modes) if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                        mode.sourceMode.position.x -= origin.x;
                        mode.sourceMode.position.y -= origin.y;
                    }
                }
            } else {
                bool attached = false;
                for (auto path : all.paths) {
                    if (!matches(path)) continue;
                    const bool used = std::any_of(current.paths.begin(), current.paths.end(), [&](const auto& p) {
                        return same_target(p, path) ||
                            (p.sourceInfo.adapterId.LowPart == path.sourceInfo.adapterId.LowPart &&
                             p.sourceInfo.adapterId.HighPart == path.sourceInfo.adapterId.HighPart &&
                             p.sourceInfo.id == path.sourceInfo.id);
                    });
                    if (used) continue;
                    DISPLAYCONFIG_MODE_INFO source{};
                    source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
                    source.id = path.sourceInfo.id;
                    source.adapterId = path.sourceInfo.adapterId;
                    source.sourceMode.width = 1920;
                    source.sourceMode.height = 1080;
                    source.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
                    for (const auto& p : current.paths) {
                        const auto& mode = current.modes[p.sourceInfo.modeInfoIdx].sourceMode;
                        source.sourceMode.position.x = std::max(source.sourceMode.position.x,
                            mode.position.x + static_cast<LONG>(mode.width));
                    }
                    path.sourceInfo.modeInfoIdx = static_cast<UINT>(current.modes.size());
                    path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
                    current.paths.push_back(path);
                    current.modes.push_back(source);
                    attached = true;
                    break;
                }
                if (!attached) return fail(error, "no desktop source is available for the selected virtual output");
            }
            if (apply(current, true) != ERROR_SUCCESS) return fail(error, "cannot apply the selected output state");
        }
        std::vector<VirtualDisplayOutput> outputs;
        if (!enumerate_virtual_display_outputs(outputs, error)) return false;
        monitors->clear();
        bool verified = false;
        for (const auto& output : outputs) {
            if (output.active) monitors->push_back(output.identity);
            if (_wcsicmp(output.identity.c_str(), monitor.c_str()) == 0) verified = output.active == enabled;
        }
        if (!verified) return fail(error, "Windows did not apply the selected output state");
        return intact(error);
    }

    bool prepare(std::wstring* device, std::string* error) override {
        auto provider = make_virtual_display_provider(VirtualDisplayProviderKind::Usbmmidd, package_);
        const auto status = provider->status();
        if (status.state != VirtualDisplayState::Active && status.state != VirtualDisplayState::Installed)
            return fail(error, status.detail);
        if (status.active_monitors == 0) {
            VirtualDisplayInstallRecord record;
            if (!read_install_record(&record) || !record.owned)
                return fail(error, "enable an existing virtual display or install the driver through chast first");
            if (!provider->set_monitor_count(1, error)) return false;
        }
        Snapshot current;
        bool attempted_attach = false;
        for (int attempt = 0; attempt < 30; ++attempt) {
            if (!guard_alive()) return fail(error, "display recovery helper exited");
            if (display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error)) {
                for (const auto& path : current.paths) {
                    const auto monitor = monitor_path(path);
                    if (is_usbmmidd_monitor(monitor)) {
                        monitor_ = monitor;
                        *device = monitor;
                        return true;
                    }
                }
                // A previous private session leaves its temporary monitor
                // detached. Reattach it beside the physical desktop before
                // proving GPU capture; never remove another app's monitor.
                if (!attempted_attach && attempt >= 5) {
                    attempted_attach = true;
                    if (!attach_virtual_display_outputs(1, error)) return false;
                }
            }
            Sleep(100);
        }
        return fail(error, "the virtual monitor has no active desktop output");
    }

    bool isolate(std::string* error) override {
        if (!guard_alive()) return fail(error, "display recovery helper exited before activation");
        Snapshot current, isolated;
        if (!display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error)) return false;
        size_t index = current.paths.size();
        for (size_t i = 0; i < current.paths.size(); ++i) {
            if (_wcsicmp(monitor_path(current.paths[i]).c_str(), monitor_.c_str()) == 0) index = i;
        }
        std::string normalized;
        if (!parse_virtual_display_resolution(resolution_, normalized))
            return fail(error, "invalid virtual display resolution");
        const size_t split = normalized.find('x');
        const auto provider = make_virtual_display_provider(VirtualDisplayProviderKind::Usbmmidd, package_);
        const auto mode = select_virtual_display_mode(provider->modes(),
            static_cast<uint32_t>(std::stoul(normalized.substr(0, split))),
            static_cast<uint32_t>(std::stoul(normalized.substr(split + 1))));
        if (!display_topology::isolate(current, index, mode, isolated, error)) return false;
        const LONG code = apply(isolated, true);
        if (code != ERROR_SUCCESS) return fail(error, "cannot isolate virtual display (" +
                                              std::to_string(code) + ")");
        return intact(error);
    }

    bool restore(std::string* error) override {
        if (!guard_.value) return true;
        SetEvent(restore_.value);
        if (WaitForSingleObject(guard_.value, 10000) != WAIT_OBJECT_0)
            return fail(error, "the recovery helper is retrying the original display topology");
        DWORD code = 1;
        GetExitCodeProcess(guard_.value, &code);
        if (code != 0 && !restore_snapshot(saved_)) return fail(error,
            "the display recovery helper failed (" + std::to_string(code) + ")");
        guard_.reset();
        restore_.reset();
        monitor_.clear();
        return true;
    }

    bool intact(std::string* error) override {
        if (!guard_alive()) return fail(error, "display recovery helper is no longer running");
        Snapshot current;
        if (!display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error)) return false;
        if (management_) return display_topology::valid(current);
        if (current.paths.size() != 1 ||
            _wcsicmp(monitor_path(current.paths[0]).c_str(), monitor_.c_str()) != 0)
            return fail(error, "the private display topology changed");
        return true;
    }
private:
    bool guard_alive() const {
        return guard_.value && WaitForSingleObject(guard_.value, 0) == WAIT_TIMEOUT;
    }
    std::string resolution_;
    std::wstring package_;
    std::wstring monitor_;
    bool management_ = false;
    Snapshot saved_;
    Handle guard_, restore_;
};

bool parse_handle(const wchar_t* text, HANDLE* handle) {
    if (!text || !*text) return false;
    uintptr_t value = 0;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9' || value > (UINTPTR_MAX - (*p - L'0')) / 10) return false;
        value = value * 10 + (*p - L'0');
    }
    if (!value || value == UINTPTR_MAX) return false;
    *handle = reinterpret_cast<HANDLE>(value);
    DWORD flags = 0;
    return GetHandleInformation(*handle, &flags) != FALSE;
}
}  // namespace

std::unique_ptr<DisplayPrivacyBackend> make_display_privacy_backend(
    const std::string& resolution, const std::wstring& package_dir) {
    return std::make_unique<WindowsDisplayPrivacy>(resolution, package_dir);
}

std::unique_ptr<DisplayPrivacyBackend> make_display_management_backend(const std::wstring& package_dir) {
    return std::make_unique<WindowsDisplayPrivacy>("1920x1080", package_dir, true);
}

bool enumerate_virtual_display_outputs(std::vector<VirtualDisplayOutput>& outputs, std::string* error) {
    Snapshot all, active;
    if (!display_topology::query(all, QDC_ALL_PATHS, error) ||
        !display_topology::query(active, QDC_ONLY_ACTIVE_PATHS, error)) return false;
    outputs.clear();
    for (const auto& path : all.paths) {
        if (!path.targetInfo.targetAvailable) continue;
        const auto identity = monitor_path(path);
        if (!is_usbmmidd_monitor(identity) || std::any_of(outputs.begin(), outputs.end(), [&](const auto& output) {
                return _wcsicmp(output.identity.c_str(), identity.c_str()) == 0;
            })) continue;
        outputs.push_back({identity, std::any_of(active.paths.begin(), active.paths.end(), [&](const auto& p) {
            return same_target(p, path);
        })});
    }
    if (outputs.size() > kMaxVirtualDisplayMonitors) return fail(error, "too many virtual outputs for this provider");
    return true;
}

uint32_t attached_virtual_display_count() {
    Snapshot current;
    std::string ignored;
    if (!display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, &ignored)) return 0;
    return static_cast<uint32_t>(std::count_if(current.paths.begin(), current.paths.end(), [](const auto& path) {
        return is_usbmmidd_monitor(monitor_path(path));
    }));
}

bool attach_virtual_display_outputs(uint32_t count, std::string* error) {
    if (!count || count > kMaxVirtualDisplayMonitors) return fail(error, "invalid virtual monitor count");
    const auto probe = probe_virtual_display();
    if (!probe.scan_error.empty() || !probe.conflicting_driver.empty())
        return fail(error, "virtual display scan failed or another driver is present");
    Snapshot original;
    if (!display_topology::query(original, QDC_ONLY_ACTIVE_PATHS, error)) return false;
    for (uint32_t attempt = 0; attempt < count; ++attempt) {
        Snapshot current, all;
        if (!display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error)) break;
        uint32_t active = 0;
        for (const auto& path : current.paths) if (is_usbmmidd_monitor(monitor_path(path))) ++active;
        if (active >= count) return true;
        if (!display_topology::query(all, QDC_ALL_PATHS, error)) break;
        bool attached = false;
        for (auto path : all.paths) {
            if (!path.targetInfo.targetAvailable || !is_usbmmidd_monitor(monitor_path(path))) continue;
            const bool used = std::any_of(current.paths.begin(), current.paths.end(), [&](const auto& p) {
                return same_target(p, path) ||
                    (p.sourceInfo.adapterId.LowPart == path.sourceInfo.adapterId.LowPart &&
                     p.sourceInfo.adapterId.HighPart == path.sourceInfo.adapterId.HighPart &&
                     p.sourceInfo.id == path.sourceInfo.id);
            });
            if (used) continue;
            DISPLAYCONFIG_MODE_INFO source{};
            source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
            source.id = path.sourceInfo.id;
            source.adapterId = path.sourceInfo.adapterId;
            source.sourceMode.width = 1920;
            source.sourceMode.height = 1080;
            source.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
            for (const auto& mode : current.modes) {
                if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                    source.sourceMode.position.x = std::max(source.sourceMode.position.x,
                        mode.sourceMode.position.x + static_cast<LONG>(mode.sourceMode.width));
            }
            path.sourceInfo.modeInfoIdx = static_cast<UINT>(current.modes.size());
            path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            path.flags = DISPLAYCONFIG_PATH_ACTIVE;
            current.paths.push_back(path);
            current.modes.push_back(source);
            const LONG code = apply(current, true);
            if (code != ERROR_SUCCESS) {
                if (error) *error = "cannot attach virtual monitor (" + std::to_string(code) + ")";
                break;
            }
            attached = true;
            break;
        }
        if (!attached) break;
    }
    Snapshot current;
    if (display_topology::query(current, QDC_ONLY_ACTIVE_PATHS, error)) {
        uint32_t active = 0;
        for (const auto& path : current.paths) if (is_usbmmidd_monitor(monitor_path(path))) ++active;
        if (active >= count) return true;
    }
    if (!original.paths.empty()) restore_snapshot(original);
    return fail(error, "could not attach the requested virtual monitors; original topology restored where possible");
}

int run_display_recovery_guard(int argc, wchar_t** argv) {
    if (argc != 6) return 2;
    std::array<Handle, 5> handles;
    for (size_t i = 0; i < handles.size(); ++i) {
        if (!parse_handle(argv[i + 1], &handles[i].value)) return 2;
    }
    DWORD parent_session = 0, own_session = 0;
    const DWORD parent_pid = GetProcessId(handles[0].value);
    if (!parent_pid || !ProcessIdToSessionId(parent_pid, &parent_session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || parent_session != own_session) return 2;
    const auto* data = static_cast<const RecoveryData*>(MapViewOfFile(handles[1].value,
        FILE_MAP_READ, 0, 0, sizeof(RecoveryData)));
    if (!data) return 3;
    Snapshot snapshot;
    if (data->magic == 0x43564431 && data->paths <= 64 && data->modes <= 256) {
        snapshot.paths.assign(data->path, data->path + data->paths);
        snapshot.modes.assign(data->mode, data->mode + data->modes);
    }
    UnmapViewOfFile(data);
    if (!display_topology::valid(snapshot)) return 3;
    if (!SetEvent(handles[2].value)) return 4;
    HANDLE waits[] = {handles[0].value, handles[3].value};
    const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (result != WAIT_OBJECT_0 && result != WAIT_OBJECT_0 + 1) return 4;
    // The guardian deliberately outlives the host and retries through a
    // disconnected monitor / locked desktop until a usable original output
    // is available. Restarting Windows uses its unchanged persistent topology.
    while (!restore_snapshot(snapshot)) Sleep(1000);
    return 0;
}
}  // namespace chast
