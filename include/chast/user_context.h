#pragma once

#include <windows.h>

namespace chast {

// Thread-local access to the original user's certificate store, DPAPI and
// settings after a UAC handoff. Capture/input retain the elevated process
// identity: keep this scope around credential and profile operations only.
class ScopedUserContext {
public:
    explicit ScopedUserContext(HANDLE token);
    ~ScopedUserContext();
    ScopedUserContext(const ScopedUserContext&) = delete;
    ScopedUserContext& operator=(const ScopedUserContext&) = delete;
    bool ok() const { return error_ == ERROR_SUCCESS; }
    DWORD error() const { return error_; }
private:
    HANDLE previous_ = nullptr;
    DWORD error_ = ERROR_SUCCESS;
    bool changed_ = false;
};

bool process_is_elevated();

} // namespace chast
