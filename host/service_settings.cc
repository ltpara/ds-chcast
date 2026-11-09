#define NOMINMAX
#include "service_settings.h"
#include "service.h"
#include <windows.h>
#include <sddl.h>
#include <algorithm>

namespace chast::host {
bool merge_service_settings(const HostSettings& original, const HostSettings& edited,
                            const HostSettings& current, ServiceSettingsPage page,
                            HostSettings& merged, std::string* error) {
    if (error) error->clear();
    HostSettings result = current;
    const auto update = [&](const auto& before, const auto& after, auto& now, const char* name) {
        if (before == after) return true;
        if (now != before && now != after) {
            if (error) *error = "Settings changed while this page was open: " + std::string(name) + ". Reopen the page and try again.";
            return false;
        }
        now = after;
        return true;
    };
#define APPLY(field) if (!update(original.field, edited.field, result.field, #field)) return false
    if (page == ServiceSettingsPage::Security) {
        APPLY(auth_mode); APPLY(password_hash); APPLY(totp_secret); APPLY(ip_whitelist);
        APPLY(allow_input); APPLY(allow_clipboard); APPLY(allow_file_transfer);
        APPLY(clipboard_direction);
        APPLY(allow_port_forward); APPLY(allow_terminal); APPLY(allow_terminal_persist);
        APPLY(allow_camera); APPLY(allow_record); APPLY(allow_restart); APPLY(allow_block_input);
        APPLY(record_incoming); APPLY(record_directory);
        APPLY(allow_os_login); APPLY(allow_session_select); APPLY(allow_remote_config);
        APPLY(lock_after_session_end); APPLY(keep_awake); APPLY(auto_disconnect_minutes); APPLY(trusted_device_days);
        APPLY(allow_audio); APPLY(allow_microphone); APPLY(allow_privacy_mode); APPLY(allow_permission_change);
        APPLY(file_receive_directory); APPLY(audio_device); APPLY(audio_aec); APPLY(audio_silence_gate_ms);
        APPLY(virtual_display); APPLY(virtual_display_monitors); APPLY(virtual_display_resolution); APPLY(privacy_mode_impl);
        APPLY(token_auth); APPLY(token_mode); APPLY(access_token);
        if (original.password_hash != edited.password_hash) result.trusted_devices.clear();
        else {
            // This page can revoke devices, but cannot add them. Keep entries
            // admitted while it was open and newer last-seen timestamps.
            for (const auto& device : original.trusted_devices) {
                if (std::none_of(edited.trusted_devices.begin(), edited.trusted_devices.end(),
                    [&](const auto& kept) { return kept.public_key == device.public_key; })) {
                    std::erase_if(result.trusted_devices, [&](const auto& live) { return live.public_key == device.public_key; });
                }
            }
        }
        result.require_approval = result.auth_mode != HostAuthMode::Password;
        if (result.auth_mode == HostAuthMode::Password && result.password_hash.empty()) {
            if (error) *error = "Password authentication requires a permanent password in service mode.";
            return false;
        }
    } else {
        APPLY(tunnel); APPLY(server_url); APPLY(server_key); APPLY(host_key); APPLY(proxy);
        APPLY(tunnel_id); APPLY(tunnel_old_id); APPLY(tunnel_key); APPLY(tunnel_listen);
    }
#undef APPLY
    merged = std::move(result);
    return true;
}

bool service_settings_admin() {
    BYTE sid[SECURITY_MAX_SID_SIZE]{};
    DWORD size = sizeof(sid);
    BOOL member = FALSE;
    return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &size) &&
        CheckTokenMembership(nullptr, sid, &member) && member;
}
namespace {
struct ServiceHandle {
    SC_HANDLE handle = nullptr;
    ~ServiceHandle() { if (handle) CloseServiceHandle(handle); }
};
bool wait_state(SC_HANDLE service, DWORD wanted, DWORD timeout) {
    const auto until = GetTickCount64() + timeout;
    for (;;) {
        SERVICE_STATUS state{};
        if (!QueryServiceStatus(service, &state)) return false;
        if (state.dwCurrentState == wanted) return true;
        if ((wanted == SERVICE_RUNNING && state.dwCurrentState == SERVICE_STOPPED) || GetTickCount64() >= until) return false;
        Sleep(100);
    }
}
ServiceSettingsResult save_to_service(SC_HANDLE service, const std::string& path,
                                      const HostSettings& original, const HostSettings& edited,
                                      ServiceSettingsPage page) {
    ServiceSettingsResult result;
    SERVICE_STATUS state{};
    if (!QueryServiceStatus(service, &state)) { result.error = "Cannot read the host service state."; return result; }
    if (state.dwCurrentState == SERVICE_START_PENDING && !wait_state(service, SERVICE_RUNNING, 20000)) {
        result.error = "The host service is still starting; settings were not saved."; return result;
    }
    if (!QueryServiceStatus(service, &state)) { result.error = "Cannot read the host service state."; return result; }
    const bool was_running = state.dwCurrentState != SERVICE_STOPPED && state.dwCurrentState != SERVICE_STOP_PENDING;
    if (state.dwCurrentState != SERVICE_STOPPED) {
        if (state.dwCurrentState != SERVICE_STOP_PENDING && !ControlService(service, SERVICE_CONTROL_STOP, &state)) {
            result.error = "Cannot stop the host service; settings were not saved."; return result;
        }
        if (!wait_state(service, SERVICE_STOPPED, 30000)) {
            result.error = "The host service did not finish stopping; settings were not saved."; return result;
        }
    }
    // All worker callbacks have finished writing. No remote setting/token
    // update can overwrite this editor's save after the worker has exited.
    const auto latest = load_host_settings(path);
    HostSettings merged;
    if (!latest.existed || !latest.warning.empty()) {
        result.error = "Cannot safely read the machine settings; settings were not saved.";
    } else if (merge_service_settings(original, edited, latest.settings, page, merged, &result.error)) {
        result.saved = save_host_settings(path, merged, &result.error);
    }
    if (was_running) {
        const bool started = StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        result.running = started && wait_state(service, SERVICE_RUNNING, 20000);
        if (!result.running) {
            if (!result.error.empty()) result.error += " ";
            result.error += result.saved ? "Settings were saved, but the host service did not restart. Start it from Windows Services."
                                         : "The previous configuration remains on disk, but the host service did not restart.";
        }
    }
    return result;
}
}
ServiceSettingsResult save_service_settings(const HostSettings& original, const HostSettings& edited,
                                            ServiceSettingsPage page) {
    if (!service_settings_admin()) return {false, false, "Administrator permission is required to edit machine settings."};
    // Serialize independent elevated editors, including different Windows
    // sessions. A failed/abandoned editor never leaves the lock permanently held.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr)) return {false, false, "Cannot create the machine settings lock policy."};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE mutex = CreateMutexW(&attributes, FALSE, L"Global\\chast-machine-settings-editor");
    LocalFree(descriptor);
    if (!mutex) return {false, false, "Cannot open the machine settings lock."};
    const auto locked = WaitForSingleObject(mutex, 0);
    if (locked != WAIT_OBJECT_0 && locked != WAIT_ABANDONED) {
        CloseHandle(mutex); return {false, false, "Another operator is saving machine settings. Try again when it finishes."};
    }
    ServiceSettingsResult result;
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    ServiceHandle service{manager.handle ? OpenServiceW(manager.handle, kServiceName,
        SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START) : nullptr};
    const auto path = machine_settings_path();
    if (!service.handle || path.empty()) result.error = "Cannot open the installed host service and its machine settings.";
    else result = save_to_service(service.handle, path, original, edited, page);
    ReleaseMutex(mutex); CloseHandle(mutex);
    return result;
}
}
