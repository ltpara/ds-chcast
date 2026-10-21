#pragma once

// Owns the chast-tunnel host child process for the tray application.
//
// The agent is a separate Go executable: the tray starts it next to itself,
// forwards its stdout JSON events to the window and stops it when sharing
// ends. Keeping the process boundary means the NAT logic has exactly one
// implementation shared with the viewer, and a crashing agent cannot take the
// host UI with it.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "chast/host_settings.h"

namespace chast::host {

struct TunnelEvent {
    std::string event;
    std::string listen;
    std::string host;
    std::string path;
    std::string remote;
    std::string punch;
    std::string relay;
};

// Reads a top-level string field from one flat JSON object. Empty when the
// field is absent. The agent only emits addresses and identifiers, so this
// deliberately does not implement nesting, numbers or arrays.
std::string json_string_field(const std::string& json, const std::string& name);

// Parses one line from chast-tunnel stdout. False for logs, malformed lines
// and event names this build does not know.
bool parse_tunnel_event(const std::string& line, TunnelEvent& event);

// Sibling chast-tunnel.exe of the given host executable. Split out so the
// path rule is testable without starting a process.
std::wstring tunnel_executable_for(const std::wstring& host_executable);

// The path the tray will launch: CHAST_TUNNEL when set, otherwise the sibling
// of chast-host.exe.
std::wstring tunnel_executable();

// Complete command line for host mode, with arguments quoted for CreateProcess.
std::wstring tunnel_command_line(const std::wstring& executable,
                                 const HostSettings& settings, uint16_t local_port,
                                 const std::string& fingerprint);

// Checks the service fields an enabled tunnel needs. Returns a Chinese message
// for the dialog or log. A disabled tunnel always validates.
bool validate_tunnel_settings(const HostSettings& settings, std::string* error);

class TunnelProcess {
public:
    using EventCallback = std::function<void(const TunnelEvent&)>;
    using LogCallback = std::function<void(const std::string&)>;
    using ExitCallback = std::function<void(uint32_t)>;

    TunnelProcess() = default;
    ~TunnelProcess();
    TunnelProcess(const TunnelProcess&) = delete;
    TunnelProcess& operator=(const TunnelProcess&) = delete;

    // Starts the agent and immediately returns; readiness arrives through
    // on_event. Callbacks run on reader threads.
    bool start(const HostSettings& settings, uint16_t local_port, const std::string& fingerprint,
               EventCallback on_event, LogCallback on_log, ExitCallback on_exit,
               std::string* error);
    // Terminates the agent and joins its reader threads. Safe to call twice.
    void stop();
    bool running() const;
    uint32_t exit_code() const;

private:
    static DWORD WINAPI stdout_entry(void* self);
    static DWORD WINAPI stderr_entry(void* self);
    void read_stdout();
    void read_stderr();
    void read_lines(HANDLE pipe, bool events);
    void publish_line(const std::string& line, bool events);
    void finish();

    HANDLE process_ = nullptr;
    // KILL_ON_JOB_CLOSE. The handle lives in this process, so a crashed host
    // takes the agent down with it instead of leaving it bound to the QUIC
    // port it inherited.
    HANDLE job_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;
    HANDLE stdout_thread_ = nullptr;
    HANDLE stderr_thread_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<uint32_t> exit_code_{0};
    EventCallback on_event_;
    LogCallback on_log_;
    ExitCallback on_exit_;
};

}  // namespace chast::host
