#pragma once

#include <string_view>

namespace chast::host::detail {

inline constexpr std::wstring_view kMachineStateSystemSid = L"S-1-5-18";
inline constexpr std::wstring_view kMachineStateAdministratorsSid = L"S-1-5-32-544";

inline bool machine_state_owner_is_system_or_admin(std::wstring_view owner_sid) noexcept {
    return owner_sid == kMachineStateSystemSid ||
           owner_sid == kMachineStateAdministratorsSid;
}

inline bool machine_state_owner_trusted_for_migration(
    std::wstring_view owner_sid, std::wstring_view installer_sid,
    bool installer_elevated) noexcept {
    return machine_state_owner_is_system_or_admin(owner_sid) ||
           (installer_elevated && !installer_sid.empty() && owner_sid == installer_sid);
}

}  // namespace chast::host::detail
