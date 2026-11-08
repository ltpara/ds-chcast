#define NOMINMAX
#include "windows_sessions.h"
#include "platform.h"
#include <windows.h>
#include <wtsapi32.h>
#include <algorithm>
#include <charconv>

namespace chast::host {
const WindowsSession* find_windows_session(const std::vector<WindowsSession>& sessions, uint32_t id) {
    if (!id || id == UINT32_MAX) return nullptr;
    const auto found = std::find_if(sessions.begin(), sessions.end(), [&](const auto& session) {
        return session.id == id && session.logon_id && !session.user.empty();
    });
    return found == sessions.end() ? nullptr : &*found;
}
WindowsSessionTarget choose_windows_session(const std::vector<WindowsSession>& sessions,
                                           uint32_t console, uint32_t current,
                                           WindowsSessionTarget& selected) {
    const auto* explicit_target = find_windows_session(sessions, selected.id);
    if (explicit_target && explicit_target->logon_id == selected.logon_id) return selected;
    selected = {};
    const auto target = [](const WindowsSession& session) { return WindowsSessionTarget{session.id, session.logon_id}; };
    if (const auto* session = find_windows_session(sessions, console)) return target(*session);
    if (const auto* session = find_windows_session(sessions, current)) return target(*session);
    WindowsSessionTarget first;
    for (const auto& session : sessions) {
        if (session.id && session.id != UINT32_MAX && session.logon_id && !session.user.empty() &&
            (!first.id || session.id < first.id)) first = target(session);
    }
    return first;
}
std::string format_session_switch(const SessionSwitchRequest& request) {
    return "1:" + std::to_string(request.worker_pid) + ":" + std::to_string(request.session_id) + ":" + std::to_string(request.logon_id);
}
bool parse_session_switch(std::string_view text, SessionSwitchRequest& request) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    if (!text.starts_with("1:")) return false;
    text.remove_prefix(2);
    const auto number = [&](auto& value, bool last) {
        const auto separator = last ? text.size() : text.find(':');
        if (separator == std::string_view::npos || !separator) return false;
        const auto parsed = std::from_chars(text.data(), text.data() + separator, value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + separator || !value) return false;
        text.remove_prefix(separator + (last ? 0 : 1));
        return true;
    };
    SessionSwitchRequest parsed;
    if (!number(parsed.worker_pid, false) || !number(parsed.session_id, false) || !number(parsed.logon_id, true) ||
        parsed.worker_pid == UINT32_MAX || parsed.session_id == UINT32_MAX) return false;
    request = parsed;
    return true;
}
bool read_windows_sessions(std::vector<WindowsSession>& sessions, std::string* error) {
    if (error) error->clear();
    PWTS_SESSION_INFOW entries = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &entries, &count)) {
        if (error) *error = "Cannot enumerate Windows sessions (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    std::vector<WindowsSession> found;
    DWORD failure = ERROR_SUCCESS;
    for (DWORD i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        if (!entry.SessionId || entry.SessionId == UINT32_MAX || entry.State != WTSActive) continue;
        LPWSTR name = nullptr;
        DWORD bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, entry.SessionId, WTSUserName, &name, &bytes)) {
            failure = GetLastError(); break;
        }
        const auto user = name ? narrow(name) : std::string{};
        WTSFreeMemory(name);
        if (user.empty()) continue;
        HANDLE token = nullptr;
        if (!WTSQueryUserToken(entry.SessionId, &token)) { failure = GetLastError(); break; }
        TOKEN_STATISTICS statistics{};
        const BOOL read = GetTokenInformation(token, TokenStatistics, &statistics, sizeof(statistics), &bytes);
        failure = read ? ERROR_SUCCESS : GetLastError();
        CloseHandle(token);
        if (!read) break;
        const auto logon = (uint64_t(static_cast<uint32_t>(statistics.AuthenticationId.HighPart)) << 32) |
                           statistics.AuthenticationId.LowPart;
        if (!logon) { failure = ERROR_INVALID_DATA; break; }
        found.push_back({entry.SessionId, logon, user});
    }
    WTSFreeMemory(entries);
    if (failure != ERROR_SUCCESS) {
        if (error) *error = "Cannot read a complete Windows logon snapshot (error " + std::to_string(failure) + ")";
        return false;
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    sessions = std::move(found);
    return true;
}
}
