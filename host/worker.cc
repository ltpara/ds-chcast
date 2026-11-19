// Session worker for the Windows service.
//
// The service runs in session 0, where DXGI capture and SendInput cannot reach
// the interactive desktop, so it launches this process with the session's
// winlogon token. The worker owns the HostServer and the tunnel child for as
// long as that session is active and stops both when the service signals.

#include "service.h"
#include "windows_sessions.h"

#include <windows.h>
#include <wtsapi32.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "chast/capture.h"
#include "chast/host_server.h"
#include "chast/host_client_strategy.h"
#include "chast/identity.h"
#include "chast/live_connection.h"
#include "chast/machine_state_security.h"
#include "chast/virtual_display.h"
#include "platform.h"
#include "service_log.h"
#include "service_control.h"
#include "tunnel.h"

namespace chast::host {

namespace {

struct WorkerContext {
    std::string log_path;
    std::string settings_path;
    std::mutex settings_mutex;
    HostSettings settings;
};

WorkerContext* g_worker = nullptr;

void worker_log(const std::string& message) {
    log_line(g_worker->log_path, message);
}

void worker_event(std::string_view event, std::initializer_list<LogField> fields = {}) {
    log_event(g_worker->log_path, event, fields);
}

void save_rotated_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_worker->settings_mutex);
    if (g_worker->settings.access_token == token) {
        return;
    }
    g_worker->settings.access_token = token;
    if (g_worker->settings_path.empty()) {
        return;
    }
    std::string error;
    if (!save_host_settings(g_worker->settings_path, g_worker->settings, &error)) {
        worker_event("settings.token_save_failed", {{"error", error}});
    }
}

void save_trusted_devices(const std::vector<TrustedDevice>& devices) {
    std::lock_guard<std::mutex> lock(g_worker->settings_mutex);
    g_worker->settings.trusted_devices = devices;
    if (g_worker->settings_path.empty()) {
        return;
    }
    std::string error;
    if (!save_host_settings(g_worker->settings_path, g_worker->settings, &error)) {
        worker_event("settings.trusted_devices_save_failed", {{"error", error}});
    }
}

void save_password_hash(const std::string& hash) {
    std::lock_guard<std::mutex> lock(g_worker->settings_mutex);
    if (g_worker->settings.password_hash == hash) {
        return;
    }
    g_worker->settings.password_hash = hash;
    if (g_worker->settings_path.empty()) {
        return;
    }
    std::string error;
    if (!save_host_settings(g_worker->settings_path, g_worker->settings, &error)) {
        worker_event("settings.password_hash_save_failed", {{"error", error}});
    }
}

// A viewer with kPermissionRemoteConfig changed one of the exposed switches.
// Only those fields are taken from the worker's copy; credentials and the
// listener's other state stay with the file already on disk.
void save_remote_settings(const HostSettings& update) {
    std::lock_guard<std::mutex> lock(g_worker->settings_mutex);
    HostSettings& settings = g_worker->settings;
    if (settings.lock_after_session_end != update.lock_after_session_end) {
        mark_host_preference(settings, HostPreference::LockAfterSessionEnd);
    }
    settings.lock_after_session_end = update.lock_after_session_end;
    settings.allow_clipboard = update.allow_clipboard;
    settings.allow_file_transfer = update.allow_file_transfer;
    settings.allow_audio = update.allow_audio;
    settings.allow_microphone = update.allow_microphone;
    settings.allow_privacy_mode = update.allow_privacy_mode;
    settings.allow_restart = update.allow_restart;
    settings.allow_block_input = update.allow_block_input;
    settings.allow_os_login = update.allow_os_login;
    settings.allow_session_select = update.allow_session_select;
    settings.allow_terminal = update.allow_terminal;
    settings.allow_port_forward = update.allow_port_forward;
    settings.allow_camera = update.allow_camera;
    settings.allow_record = update.allow_record;
    if (g_worker->settings_path.empty()) {
        return;
    }
    std::string error;
    if (!save_host_settings(g_worker->settings_path, settings, &error)) {
        worker_event("settings.remote_save_failed", {{"error", error}});
    }
}

void clear_migrated_identity(const std::string& old_id) {
    std::lock_guard<std::mutex> lock(g_worker->settings_mutex);
    if (g_worker->settings.tunnel_old_id != old_id) {
        return;
    }
    g_worker->settings.tunnel_old_id.clear();
    if (g_worker->settings_path.empty()) {
        return;
    }
    std::string error;
    if (!save_host_settings(g_worker->settings_path, g_worker->settings, &error)) {
        worker_event("settings.migrated_id_clear_failed", {{"error", error}});
    }
}

void log_tunnel_event(HostServer& server, const TunnelEvent& event) {
    if (event.event == "ready") {
        if (!event.migrated_from.empty()) {
            clear_migrated_identity(event.migrated_from);
        }
        worker_event("tunnel.online",
                     {{"host", event.host},
                      {"listen", event.listen},
                      {"relay", event.relay},
                      {"migrated_from", event.migrated_from}});
    } else if (event.event == "viewer") {
        worker_event("tunnel.viewer",
                     {{"path", event.path.empty() ? "unknown" : event.path},
                      {"remote", event.remote}});
    } else if (event.event == "relay") {
        worker_event("tunnel.relay", {{"relay", event.relay}});
    } else if (event.event == "policy") {
        worker_event("tunnel.policy",
                     {{"version", event.has_policy
                                     ? std::to_string(event.policy.policy_version)
                                     : std::string()},
                      {"managed", event.has_policy ? "true" : "false"},
                      {"reason", event.reason}});
        if (event.has_policy) {
            server.revoke_managed_sessions(
                event.reason.empty() ? std::string("device policy changed") : event.reason,
                event.policy.policy_version);
        }
    } else if (event.event == "revoke") {
        worker_event("tunnel.revoke", {{"kid", event.kid}, {"reason", event.reason}});
        if (!event.kid.empty()) {
            server.revoke_managed_sessions(
                event.reason.empty() ? std::string("issuer key revoked") : event.reason, 0,
                event.kid);
        }
    }
    if (event.has_policy && event.event != "viewer") {
        std::string ack_path;
        std::string state_path;
        {
            std::lock_guard<std::mutex> lock(g_worker->settings_mutex);
            ack_path = g_worker->settings.policy_ack;
            state_path = g_worker->settings.policy_state;
        }
        if (!ack_path.empty() && !state_path.empty()) {
            std::string error;
            if (!write_managed_policy_ack(ack_path, state_path, event.policy.mode,
                                          event.policy.policy_version,
                                          event.policy.permission_ceiling,
                                          event.policy.updated_at_ms, &error)) {
                worker_event("tunnel.policy_ack_failed", {{"error", error}});
            }
        }
    }
}

// Lists the sessions a viewer may switch to. Only WTSActive sessions with a
// user are returned: DXGI has no desktop to capture in a disconnected session,
// and the name is what the viewer shows. `current` marks this worker's own
// session so the UI can leave it out of the switch choices.
bool list_active_sessions(std::vector<RemoteSession>& sessions, std::string* error) {
    std::vector<WindowsSession> active;
    if (!read_windows_sessions(active, error)) return false;
    DWORD current = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &current);
    sessions.clear();
    // Keep this worker in bounded replies even on a host with many RDP users.
    std::stable_sort(active.begin(), active.end(), [current](const auto& a, const auto& b) {
        return (a.id == current) > (b.id == current);
    });
    for (const auto& entry : active) {
        if (sessions.size() == kMaxRemoteSessions) break;
        RemoteSession session;
        session.id = entry.id; session.user = entry.user;
        session.active = true; session.current = entry.id == current;
        sessions.push_back(std::move(session));
    }
    return true;
}

// Re-validates the target, binds it to its logon and this worker, and wakes
// the service. Atomic replacement prevents a partially written request.
bool request_session_switch(uint32_t session_id, std::string* error) {
    std::vector<WindowsSession> sessions;
    std::string list_error;
    if (!read_windows_sessions(sessions, &list_error)) {
        *error = list_error;
        return false;
    }
    const auto* target = find_windows_session(sessions, session_id);
    if (target == nullptr) {
        *error = "the session is not active any more";
        return false;
    }
    DWORD current = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &current);
    if (target->id == current) {
        return true;
    }
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kWorkerSwitchEvent);
    if (event == nullptr) {
        *error = "the service session switch channel is unavailable";
        return false;
    }
    const std::wstring path = session_switch_path();
    if (path.empty()) {
        CloseHandle(event);
        *error = "the machine settings path is unavailable";
        return false;
    }
    std::wstring temporary = path;
    temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        CloseHandle(event);
        *error = "cannot write the session switch request (error " +
                 std::to_string(GetLastError()) + ")";
        return false;
    }
    const std::string text = format_session_switch({GetCurrentProcessId(), session_id, target->logon_id});
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                                 nullptr);
    CloseHandle(file);
    if (!wrote || written != static_cast<DWORD>(text.size())) {
        DeleteFileW(temporary.c_str());
        CloseHandle(event);
        *error = "cannot write the session switch request";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        CloseHandle(event);
        *error = "cannot publish the session switch request (error " +
                 std::to_string(GetLastError()) + ")";
        return false;
    }
    const BOOL signalled = SetEvent(event);
    CloseHandle(event);
    if (!signalled) {
        *error = "the service could not be signalled for the session switch";
        return false;
    }
    worker_event("session.switch_requested", {{"session", std::to_string(session_id)}});
    return true;
}

void prepare_virtual_display_on_start(const HostSettings& settings) {
    if (settings.virtual_display == VirtualDisplayPolicy::Off) {
        return;
    }
    if (settings.virtual_display_provider == "vdd") {
        worker_event("vdisplay.unsupported", {{"provider", "vdd"}});
        return;
    }
    VirtualDisplayProviderKind kind = VirtualDisplayProviderKind::None;
    if (settings.virtual_display_provider == "usbmmidd") {
        kind = VirtualDisplayProviderKind::Usbmmidd;
    } else {
        kind = detect_virtual_display_provider();
    }
    std::unique_ptr<VirtualDisplayProvider> provider;
    if (kind != VirtualDisplayProviderKind::None) {
        provider = make_virtual_display_provider(kind, default_virtual_display_package_dir());
    }
    if (!provider) {
        const VirtualDisplayStatus status = classify_virtual_display_probe(probe_virtual_display());
        worker_event("vdisplay.unavailable", {{"detail", status.detail}});
        return;
    }
    bool active_display = false;
    for (const DisplayInfo& display : enumerate_displays()) {
        if (display.width != 0 && display.height != 0) {
            active_display = true;
            break;
        }
    }
    const VirtualDisplayPrepareResult result = prepare_virtual_display(
        settings.virtual_display, settings.virtual_display_monitors,
        settings.virtual_display_resolution, provider.get(), active_display);
    worker_event("vdisplay.prepared",
                 {{"detail", result.detail},
                  {"state", virtual_display_state_name(result.status.state)},
                  {"monitors", std::to_string(result.status.active_monitors)}});
}

bool load_worker_strategy(const HostSettings& local, HostSettings& effective,
                          HostClientStrategy& previous, HostStrategyIdentity& identity,
                          bool& loaded, bool& blocked, bool& changed,
                          std::string& error) {
    blocked = false;
    changed = false;
    effective = local;
    const std::string path = host_client_strategy_path(local);
    if (path.empty()) {
        loaded = false;
        return true;
    }
    const int path_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), nullptr, 0);
    if (path_length <= 0) {
        blocked = true;
        error = "client strategy cache path is not valid UTF-8";
        return false;
    }
    std::wstring native_path(static_cast<size_t>(path_length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                        static_cast<int>(path.size()), native_path.data(),
                        path_length);
    std::error_code ec;
    const bool present =
        std::filesystem::exists(std::filesystem::path(native_path), ec);
    if (ec) {
        blocked = true;
        error = "cannot inspect client strategy cache: " + ec.message();
        return false;
    }
    if (!present) {
        effective = loaded ? host_strategy_effective_settings(local, previous) : local;
        return true;
    }
    if (!host_strategy_identity(local, "", identity, &error)) {
        blocked = true;
        return false;
    }
    HostClientStrategy next;
    const HostClientStrategy* prior = loaded ? &previous : nullptr;
    if (!read_host_client_strategy(path, identity, prior, next, &error)) {
        blocked = true;
        if (loaded) effective = host_strategy_effective_settings(local, previous);
        return false;
    }
    if (loaded && next.revision == previous.revision &&
        next.digest == previous.digest) {
        effective = host_strategy_effective_settings(local, previous);
        return true;
    }
    changed = !loaded || next.revision != previous.revision ||
              next.digest != previous.digest;
    previous = std::move(next);
    loaded = true;
    blocked = host_strategy_has_unsupported_settings(previous);
    if (blocked)
        error = "client strategy contains settings unsupported by this worker";
    effective = host_strategy_effective_settings(local, previous);
    return true;
}

bool start_tunnel(TunnelProcess& tunnel, const HostSettings& settings,
                  const std::string& fingerprint, HostServer& server,
                  std::atomic<bool>& ended, std::atomic<uint32_t>& exit_code,
                  std::atomic<bool>& ready, std::atomic<bool>& failed) {
    ended = false;
    exit_code = 0;
    ready = false;
    failed = false;
    std::string error;
    if (!tunnel.start(settings, settings.port, fingerprint,
                      [&server, &ready](const TunnelEvent& event) {
                          if (event.event == "ready") ready = true;
                          log_tunnel_event(server, event);
                      },
                      [](const std::string& line) {
                          worker_event("tunnel.output", {{"text", line}});
                      },
                      [&ended, &exit_code, &ready, &failed](uint32_t code) {
                          ready = false;
                          failed = code != 0;
                          exit_code = code;
                          ended = true;
                      },
                      &error)) {
        failed = true;
        worker_event("tunnel.start_failed", {{"error", error}});
        return false;
    }
    worker_event("tunnel.starting", {{"device", settings.tunnel_id}});
    return true;
}

}  // namespace

int run_worker(std::string* error) {
    std::string state_error;
    if (!machine_state_directory_trusted(widen(machine_settings_directory()), &state_error) ||
        !machine_state_file_trusted(widen(machine_settings_path()), &state_error)) {
        if (error != nullptr) *error = "机器状态目录或设置文件不可信：" + state_error;
        return 1;
    }
    WorkerContext context;
    g_worker = &context;
    context.log_path = service_log_path("worker.log");
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    worker_event("worker.start",
                 {{"pid", std::to_string(GetCurrentProcessId())},
                  {"session", std::to_string(session)}});

    HostSettings settings;
    std::string settings_warning;
    std::string settings_error;
    if (!load_machine_settings(settings, &context.settings_path, &settings_warning,
                               &settings_error)) {
        worker_event("settings.error", {{"error", settings_error}});
    } else {
        worker_event("settings.loaded", {{"path", context.settings_path}});
    }
    if (!settings_warning.empty()) {
        worker_event("settings.warning", {{"text", settings_warning}});
    }
    context.settings = settings;
    HostSettings effective_settings;
    HostClientStrategy managed_strategy;
    HostStrategyIdentity strategy_identity;
    bool managed_strategy_loaded = false;
    bool strategy_blocked = false;
    bool strategy_changed = false;
    std::string strategy_error;
    load_worker_strategy(settings, effective_settings, managed_strategy,
                         strategy_identity, managed_strategy_loaded,
                         strategy_blocked, strategy_changed, strategy_error);
    if (!strategy_error.empty()) {
        worker_event("strategy.unavailable",
                     {{"error", strategy_error},
                      {"blocked", strategy_blocked ? "true" : "false"}});
    } else if (managed_strategy_loaded) {
        worker_event("strategy.loaded",
                     {{"revision", std::to_string(managed_strategy.revision)},
                      {"digest", managed_strategy.digest}});
    }

    if (settings.password_hash.empty()) {
        worker_event("auth.password_missing");
    }
    if (settings.token_auth) {
        worker_event("auth.token_enabled");
    }
    prepare_virtual_display_on_start(settings);

    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, kWorkerStopEvent);
    if (stop == nullptr) {
        stop = CreateEventW(nullptr, TRUE, FALSE, kWorkerStopEvent);
        if (stop != nullptr) {
            worker_event("events.stop_created");
        }
    }
    if (stop == nullptr) {
        *error = "无法打开停止事件（错误 " + std::to_string(GetLastError()) + "）";
        worker_event("worker.fatal", {{"error", *error}});
        g_worker = nullptr;
        return 1;
    }

    ServiceControlServer control;
    HostServerConfig config;
    config.settings = effective_settings;
    config.audio_silence_gate_ms = effective_settings.audio_silence_gate_ms;
    config.identity_scope = IdentityScope::LocalMachine;
    // Click/Both waits for an authenticated operator in this Windows session.
    // The service never creates temporary passwords or bypasses missing approval.
    config.service_mode = true;
    config.service_approval = true;
    // A locked desktop makes capture fail; the worker has to keep retrying
    // until the user unlocks instead of ending the service.
    config.max_consecutive_failures = 0;
    config.approve_client = [&control](const ClientRequest& request) { return control.approve(request); };
    config.viewer_chat_message = [&control](uint64_t id, const std::string& text) { control.receive_chat(id, text); };
    config.log = worker_log;
    config.token_changed = save_rotated_token;
    config.trusted_devices_changed = save_trusted_devices;
    config.password_hash_changed = save_password_hash;
    config.settings_changed = save_remote_settings;

    // Only a worker started by the service can hand the stream to another
    // session; a manual run must not advertise a switch it cannot perform.
    HANDLE switch_probe = OpenEventW(EVENT_MODIFY_STATE, FALSE, kWorkerSwitchEvent);
    const bool service_backend = switch_probe != nullptr;
    if (switch_probe != nullptr) {
        CloseHandle(switch_probe);
    }
    if (service_backend) {
        config.list_sessions = list_active_sessions;
        config.select_session = request_session_switch;
    } else if (settings.allow_session_select) {
        worker_event("session.select_withheld");
    }

    std::string create_error;
    std::unique_ptr<HostServer> server = HostServer::create(std::move(config), &create_error);
    if (!server) {
        *error = create_error;
        if (managed_strategy_loaded) {
            std::string receipt_error;
            if (!write_host_client_strategy_ack(
                    settings, strategy_identity, managed_strategy, false, false,
                    &receipt_error, true, false)) {
                worker_event("strategy.receipt_failed", {{"error", receipt_error}});
            }
        }
        worker_event("listener.failed", {{"error", create_error}});
        CloseHandle(stop);
        g_worker = nullptr;
        return 1;
    }
    server->set_strategy_admission_allowed(!strategy_blocked);
    // The worker is the only process reporting viewer connections while the
    // service owns the listener. It publishes beside the policy cache, which
    // the tray also uses, so exactly one controller is ever live.
    chast::host::LiveConnectionController live_connections(
        chast::host::live_connection_state_path(effective_settings));
    std::string live_state_error;
    std::string control_error;
    if (!control.start(*server, &control_error, [&context] {
        std::lock_guard lock(context.settings_mutex);
        return ServiceRegistrationInfo{context.settings.tunnel_id, context.settings.tunnel};
    })) worker_event("control.unavailable", {{"error", control_error}});
    else worker_event("control.ready");
    server->start();
    const HostStatus status = server->status();
    if (!status.fingerprint.empty()) {
        worker_event("identity.fingerprint",
                     {{"sha256", format_fingerprint(status.fingerprint)}});
    }

    bool tunnel_wanted = false;
    bool tunnel_configuration_failed = false;
    if (effective_settings.tunnel) {
        if (effective_settings.server_url.empty()) {
            worker_event("tunnel.local_only");
            tunnel_configuration_failed = true;
        } else if (std::string tunnel_error; validate_tunnel_settings(effective_settings, &tunnel_error)) {
            tunnel_wanted = true;
        } else {
            worker_event("tunnel.settings_rejected", {{"error", tunnel_error}});
            tunnel_configuration_failed = true;
        }
    }

    TunnelProcess tunnel;
    std::atomic<bool> tunnel_ended{false};
    std::atomic<uint32_t> tunnel_exit_code{0};
    std::atomic<bool> tunnel_ready{false};
    std::atomic<bool> tunnel_failed{tunnel_configuration_failed};
    bool tunnel_needs_start = tunnel_wanted;
    auto next_tunnel = std::chrono::steady_clock::now();
    auto next_strategy = std::chrono::steady_clock::now();
    if (tunnel_wanted &&
        start_tunnel(tunnel, effective_settings, status.fingerprint, *server, tunnel_ended,
                     tunnel_exit_code, tunnel_ready, tunnel_failed)) {
        tunnel_needs_start = false;
    }

    for (;;) {
        if (WaitForSingleObject(stop, 500) == WAIT_OBJECT_0) {
            break;
        }
        if (!server->running()) {
            if (managed_strategy_loaded) {
                std::string receipt_error;
                if (!write_host_client_strategy_ack(
                        settings, strategy_identity, managed_strategy, false, false,
                        &receipt_error, true, tunnel_failed.load())) {
                    worker_event("strategy.receipt_failed", {{"error", receipt_error}});
                }
            }
            worker_event("server.stopped");
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (live_connections.enabled()) {
            std::string live_error;
            if (live_connections.tick(*server, &live_error)) {
                live_state_error.clear();
            } else if (!live_error.empty() && live_error != live_state_error) {
                // Once per distinct failure: this runs every half second.
                live_state_error = live_error;
                worker_event("live_connections.unavailable", {{"error", live_error}});
            }
        }
        const bool server_ready =
            server->status().state == HostState::Listening ||
            server->status().state == HostState::Streaming;
        const bool tunnel_effectively_ready =
            !effective_settings.tunnel || tunnel_ready.load();
        if (managed_strategy_loaded) {
            std::string receipt_error;
            if (!write_host_client_strategy_ack(
                    settings, strategy_identity, managed_strategy,
                    server_ready, tunnel_effectively_ready, &receipt_error,
                    false, tunnel_failed.load())) {
                worker_event("strategy.receipt_failed", {{"error", receipt_error}});
            }
        }
        if (now >= next_strategy) {
            next_strategy = now + std::chrono::seconds(2);
            const HostSettings old_effective = effective_settings;
            HostSettings local;
            {
                std::lock_guard<std::mutex> lock(context.settings_mutex);
                local = context.settings;
            }
            bool changed = false;
            std::string read_error;
            const bool readable = load_worker_strategy(
                local, effective_settings, managed_strategy, strategy_identity,
                managed_strategy_loaded, strategy_blocked, changed, read_error);
            server->set_strategy_admission_allowed(readable && !strategy_blocked);
            server->set_strategy_locked_preferences(
                effective_settings.managed_locked_preferences);
            if (!read_error.empty()) {
                worker_event("strategy.unavailable", {{"error", read_error}});
            }
            if (changed &&
                (old_effective.keep_awake != effective_settings.keep_awake ||
                 old_effective.max_viewers != effective_settings.max_viewers ||
                 old_effective.carrier != effective_settings.carrier ||
                 old_effective.lan_discovery != effective_settings.lan_discovery)) {
                if (managed_strategy_loaded) {
                    std::string receipt_error;
                    if (!write_host_client_strategy_ack(
                            local, strategy_identity, managed_strategy, false, false,
                            &receipt_error, false, false)) {
                        worker_event("strategy.receipt_failed", {{"error", receipt_error}});
                    }
                }
                worker_event("strategy.restart_required");
                break;
            }
            if (changed) {
                server->set_next_session_strategy(
                    effective_settings.lock_after_session_end,
                    effective_settings.auto_disconnect_minutes);
            }
        }
        if (tunnel_ended.exchange(false)) {
            worker_event("tunnel.exited",
                         {{"code", std::to_string(tunnel_exit_code.load())}});
            tunnel_needs_start = true;
            next_tunnel = now + std::chrono::seconds(5);
        }
        if (!tunnel_needs_start || now < next_tunnel) {
            continue;
        }
        if (start_tunnel(tunnel, effective_settings, status.fingerprint, *server, tunnel_ended,
                         tunnel_exit_code, tunnel_ready, tunnel_failed)) {
            tunnel_needs_start = false;
        } else {
            next_tunnel = now + std::chrono::seconds(5);
        }
    }

    worker_event("worker.stopping");
    tunnel.stop();
    control.stop();
    if (live_connections.enabled()) {
        // Terminal snapshot: the operator view must not keep showing viewers
        // of a runtime that is going away, and any later command for one of
        // them has to be answered as already ended.
        std::string live_error;
        live_connections.publish_terminal(*server, &live_error);
    }
    server->stop();
    CloseHandle(stop);
    worker_event("worker.stopped");
    g_worker = nullptr;
    return 0;
}

}  // namespace chast::host
