#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chast::host {
struct WindowsSession {
    uint32_t id = 0;
    uint64_t logon_id = 0;
    std::string user;
};
struct WindowsSessionTarget {
    uint32_t id = 0;
    uint64_t logon_id = 0;
};
// SYSTEM callers only. Return active sessions with a logged-in user and its
// authentication ID. Failure leaves output unchanged, never a partial list.
bool read_windows_sessions(std::vector<WindowsSession>& sessions, std::string* error);
// Explicit selection, console, surviving current worker, then lowest ID.
// An expired explicit choice is cleared. A zero ID means no eligible session.
WindowsSessionTarget choose_windows_session(const std::vector<WindowsSession>& sessions,
                                           uint32_t console, uint32_t current,
                                           WindowsSessionTarget& selected);
const WindowsSession* find_windows_session(const std::vector<WindowsSession>& sessions, uint32_t id);
struct SessionSwitchRequest {
    uint32_t worker_pid = 0, session_id = 0;
    uint64_t logon_id = 0;
};
std::string format_session_switch(const SessionSwitchRequest& request);
bool parse_session_switch(std::string_view text, SessionSwitchRequest& request);
}
