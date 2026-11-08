#include "chast/user_context.h"
#include <exception>

namespace chast {

ScopedUserContext::ScopedUserContext(HANDLE token) {
    if (!token) return;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE | TOKEN_QUERY, TRUE, &previous_)) {
        const auto error = GetLastError();
        if (error != ERROR_NO_TOKEN) { error_ = error; return; }
    }
    if (!ImpersonateLoggedOnUser(token)) { error_ = GetLastError(); return; }
    changed_ = true;
}

ScopedUserContext::~ScopedUserContext() {
    if (changed_) {
        // Never continue under the wrong identity if restoration fails.
        const bool restored = previous_ ? SetThreadToken(nullptr, previous_) != FALSE
                                        : RevertToSelf() != FALSE;
        if (!restored) std::terminate();
    }
    if (previous_) CloseHandle(previous_);
}

bool process_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                                             &bytes) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}
} // namespace chast
