#pragma once

#include <windows.h>

#include <string>

#include "rollback_stack.h"

namespace chast::host::detail {

inline bool service_configuration_matches_target(
    DWORD current_start_type, const std::wstring& current_binary_path,
    DWORD target_start_type, const std::wstring& target_binary_path) {
    return current_start_type == target_start_type && current_binary_path == target_binary_path;
}

inline bool supported_existing_service_install_state(DWORD state) {
    return state == SERVICE_STOPPED || state == SERVICE_RUNNING;
}

template <typename StopService, typename RestoreService>
DWORD prepare_existing_service_for_update(DWORD previous_state, bool service_config_changed,
                                          bool settings_changed, RollbackStack& rollback,
                                          StopService stop_service,
                                          RestoreService restore_service) {
    if (!supported_existing_service_install_state(previous_state)) {
        return ERROR_INVALID_STATE;
    }
    if (previous_state == SERVICE_STOPPED) return ERROR_SUCCESS;

    rollback.push([restore_service, previous_state] {
        return static_cast<int>(restore_service(previous_state));
    });
    if (!service_config_changed && !settings_changed) return ERROR_SUCCESS;
    return stop_service();
}

template <typename SetDescription, typename SetDelayedStart, typename SetFailureActions>
DWORD apply_service_runtime_configuration(SetDescription set_description,
                                         SetDelayedStart set_delayed_start,
                                         SetFailureActions set_failure_actions) {
    DWORD error = set_description();
    if (error != ERROR_SUCCESS) return error;
    error = set_delayed_start();
    if (error != ERROR_SUCCESS) return error;
    return set_failure_actions();
}

template <typename Save, typename Restore>
bool save_settings_with_rollback(RollbackStack& rollback, Save save, Restore restore) {
    rollback.push([restore] { return static_cast<int>(restore()); });
    return save();
}

}  // namespace chast::host::detail
