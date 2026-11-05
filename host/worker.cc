// Session worker for the Windows service.
//
// The service runs in session 0, where DXGI capture and SendInput cannot reach
// the interactive desktop, so it launches this process with the session's
// winlogon token. The worker owns the HostServer and the tunnel child for as
// long as that session is active and stops both when the service signals.

#include "service.h"

#include <windows.h>
#include <wtsapi32.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "chast/host_server.h"
#include "chast/identity.h"
#include "platform.h"
#include "service_log.h"
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
        worker_log("cannot save the new access token: " + error);
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
        worker_log("cannot save the trusted-device list: " + error);
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
        worker_log("cannot save the upgraded password hash: " + error);
    }
}

// A viewer with kPermissionRemoteConfig changed one of the exposed switches.
// Only those fields are taken from the worker's copy; credentials and the
// listener's other state stay with the file already on disk.
void save_remote_settings(const HostSettings& update) {
    std::lock_guard<std::mutex> lock(g_worker->settings_mutex);
    HostSettings& settings = g_worker->settings;
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
        worker_log("cannot save the remote settings change: " + error);
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
        worker_log("cannot clear the migrated device id: " + error);
    }
}

void log_tunnel_event(const TunnelEvent& event) {
    if (event.event == "ready") {
        std::string line = "tunnel online";
        if (!event.host.empty()) {
            line += " id=" + event.host;
        }
        if (!event.listen.empty()) {
            line += " listen=" + event.listen;
        }
        if (!event.relay.empty()) {
            line += " relay=" + event.relay;
        }
        if (!event.migrated_from.empty()) {
            line += " migrated_from=" + event.migrated_from;
            clear_migrated_identity(event.migrated_from);
        }
        worker_log(line);
    } else if (event.event == "viewer") {
        std::string line = "viewer via " + (event.path.empty() ? "unknown" : event.path);
        if (!event.remote.empty()) {
            line += " remote=" + event.remote;
        }
        worker_log(line);
    } else if (event.event == "relay") {
        worker_log("tunnel relay address " + event.relay);
    }
}

// Lists the sessions a viewer may switch to. Only WTSActive sessions with a
// user are returned: DXGI has no desktop to capture in a disconnected session,
// and the name is what the viewer shows. `current` marks this worker's own
// session so the UI can leave it out of the switch choices.
bool list_active_sessions(std::vector<RemoteSession>& sessions, std::string* error) {
    PWTS_SESSION_INFOW entries = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &entries, &count)) {
        *error = "cannot enumerate the Windows sessions (error " +
                 std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD current = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &current);
    sessions.clear();
    for (DWORD i = 0; i < count && sessions.size() < kMaxRemoteSessions; ++i) {
        const WTS_SESSION_INFOW& entry = entries[i];
        if (entry.SessionId == 0 || entry.SessionId == 0xFFFFFFFF) {
            continue;
        }
        if (entry.State != WTSActive) {
            continue;
        }
        LPWSTR name = nullptr;
        DWORD bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, entry.SessionId,
                                         WTSUserName, &name, &bytes)) {
            continue;
        }
        std::string user = name != nullptr ? narrow(name) : std::string();
        WTSFreeMemory(name);
        RemoteSession session;
        session.id = entry.SessionId;
        session.user = std::move(user);
        session.active = true;
        session.current = entry.SessionId == current;
        sessions.push_back(std::move(session));
    }
    WTSFreeMemory(entries);
    return true;
}

// Re-validates the target, writes the id-only switch request and wakes the
// service. The file is replaced atomically and the event is signalled only
// after the write, so the service never reads a partial id.
bool request_session_switch(uint32_t session_id, std::string* error) {
    std::vector<RemoteSession> sessions;
    std::string list_error;
    if (!list_active_sessions(sessions, &list_error)) {
        *error = list_error;
        return false;
    }
    const RemoteSession* target = nullptr;
    for (const RemoteSession& entry : sessions) {
        if (entry.id == session_id) {
            target = &entry;
            break;
        }
    }
    if (target == nullptr) {
        *error = "the session is not active any more";
        return false;
    }
    if (target->current) {
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
    const std::string text = std::to_string(session_id);
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
    worker_log("session switch requested: session " + std::to_string(session_id));
    return true;
}

bool start_tunnel(TunnelProcess& tunnel, const HostSettings& settings,
                  const std::string& fingerprint, std::atomic<bool>& ended,
                  std::atomic<uint32_t>& exit_code) {
    ended = false;
    exit_code = 0;
    std::string error;
    if (!tunnel.start(settings, settings.port, fingerprint, log_tunnel_event,
                      [](const std::string& line) { worker_log("tunnel: " + line); },
                      [&ended, &exit_code](uint32_t code) {
                          exit_code = code;
                          ended = true;
                      },
                      &error)) {
        worker_log("cannot start the tunnel: " + error);
        return false;
    }
    worker_log("tunnel starting for device " + settings.tunnel_id);
    return true;
}

}  // namespace

int run_worker(std::string* error) {
    WorkerContext context;
    g_worker = &context;
    context.log_path = service_log_path("worker.log");
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    worker_log("worker starting: pid=" + std::to_string(GetCurrentProcessId()) +
               " session=" + std::to_string(session));

    HostSettings settings;
    std::string settings_warning;
    std::string settings_error;
    if (!load_machine_settings(settings, &context.settings_path, &settings_warning,
                               &settings_error)) {
        worker_log("machine settings: " + settings_error);
    } else {
        worker_log("settings: " + context.settings_path);
    }
    if (!settings_warning.empty()) {
        worker_log("settings warning: " + settings_warning);
    }
    context.settings = settings;

    if (settings.password_hash.empty()) {
        worker_log("service mode has no permanent password; every viewer will be refused "
                   "until one is set");
    }
    if (settings.auth_mode != HostAuthMode::Password) {
        worker_log("service mode ignores click approval and the temporary password; "
                   "a permanent password is required");
    }
    if (settings.token_auth) {
        worker_log("access-token authentication is enabled for automation");
    }

    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, kWorkerStopEvent);
    if (stop == nullptr) {
        stop = CreateEventW(nullptr, TRUE, FALSE, kWorkerStopEvent);
        if (stop != nullptr) {
            worker_log("stop event was missing; created it for a manual run");
        }
    }
    if (stop == nullptr) {
        *error = "无法打开停止事件（错误 " + std::to_string(GetLastError()) + "）";
        worker_log(*error);
        g_worker = nullptr;
        return 1;
    }

    HostServerConfig config;
    config.settings = settings;
    config.audio_silence_gate_ms = settings.audio_silence_gate_ms;
    config.identity_scope = IdentityScope::LocalMachine;
    // No interactive operator: the policy forces password-only mode, keeps no
    // temporary password and never asks for click approval.
    config.service_mode = true;
    // A locked desktop makes capture fail; the worker has to keep retrying
    // until the user unlocks instead of ending the service.
    config.max_consecutive_failures = 0;
    config.approve_client = nullptr;
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
        worker_log("session_select is enabled but no service switch channel is present; "
                   "the permission is withheld");
    }

    std::string create_error;
    std::unique_ptr<HostServer> server = HostServer::create(std::move(config), &create_error);
    if (!server) {
        *error = create_error;
        worker_log("cannot open the listener: " + create_error);
        CloseHandle(stop);
        g_worker = nullptr;
        return 1;
    }
    server->start();
    const HostStatus status = server->status();
    if (!status.fingerprint.empty()) {
        worker_log("identity SHA256 " + format_fingerprint(status.fingerprint));
    }

    bool tunnel_wanted = false;
    if (settings.tunnel) {
        if (settings.server_url.empty()) {
            worker_log("connection service is enabled but server_url is empty; staying local");
        } else if (std::string tunnel_error; validate_tunnel_settings(settings, &tunnel_error)) {
            tunnel_wanted = true;
        } else {
            worker_log("connection service settings rejected: " + tunnel_error);
        }
    }

    TunnelProcess tunnel;
    std::atomic<bool> tunnel_ended{false};
    std::atomic<uint32_t> tunnel_exit_code{0};
    bool tunnel_needs_start = tunnel_wanted;
    auto next_tunnel = std::chrono::steady_clock::now();
    if (tunnel_wanted &&
        start_tunnel(tunnel, settings, status.fingerprint, tunnel_ended, tunnel_exit_code)) {
        tunnel_needs_start = false;
    }

    for (;;) {
        if (WaitForSingleObject(stop, 500) == WAIT_OBJECT_0) {
            break;
        }
        if (!server->running()) {
            worker_log("the host server stopped; leaving the session");
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (tunnel_ended.exchange(false)) {
            worker_log("tunnel process exited: code=" +
                       std::to_string(tunnel_exit_code.load()));
            tunnel_needs_start = true;
            next_tunnel = now + std::chrono::seconds(5);
        }
        if (!tunnel_needs_start || now < next_tunnel) {
            continue;
        }
        if (start_tunnel(tunnel, settings, status.fingerprint, tunnel_ended, tunnel_exit_code)) {
            tunnel_needs_start = false;
        } else {
            next_tunnel = now + std::chrono::seconds(5);
        }
    }

    worker_log("worker stopping");
    tunnel.stop();
    server->stop();
    CloseHandle(stop);
    worker_log("worker stopped");
    g_worker = nullptr;
    return 0;
}

}  // namespace chast::host
