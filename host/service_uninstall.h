#pragma once

#include <windows.h>

#include "rollback_stack.h"

namespace chast::host::detail {

enum class ServiceUninstallStage {
    Complete,
    Stop,
    Firewall,
    Delete,
};

struct ServiceUninstallResult {
    ServiceUninstallStage stage = ServiceUninstallStage::Complete;
    DWORD error = ERROR_SUCCESS;
    int firewall_rollback_error = 0;
    int rollback_error = 0;
};

template <typename QueryStatus, typename StartService, typename WaitForState,
          typename ContinueService, typename PauseService>
DWORD restore_service_state_after_stop(DWORD previous_state, QueryStatus query_status,
                                       StartService start_service,
                                       WaitForState wait_for_state,
                                       ContinueService continue_service,
                                       PauseService pause_service) {
    if (previous_state == SERVICE_STOPPED || previous_state == SERVICE_STOP_PENDING) {
        return ERROR_SUCCESS;
    }
    const bool restore_paused = previous_state == SERVICE_PAUSED ||
                                previous_state == SERVICE_PAUSE_PENDING;
    if (!restore_paused && previous_state != SERVICE_RUNNING &&
        previous_state != SERVICE_START_PENDING &&
        previous_state != SERVICE_CONTINUE_PENDING) {
        return ERROR_INVALID_STATE;
    }

    const DWORD target_state = restore_paused ? SERVICE_PAUSED : SERVICE_RUNNING;
    DWORD current_state = SERVICE_STOPPED;
    const DWORD query_error = query_status(&current_state);
    if (query_error != ERROR_SUCCESS) return query_error;

    if (current_state == SERVICE_STOP_PENDING) {
        if (!wait_for_state(SERVICE_STOPPED, 30000)) return ERROR_TIMEOUT;
        current_state = SERVICE_STOPPED;
    } else if (current_state == SERVICE_START_PENDING ||
               current_state == SERVICE_CONTINUE_PENDING) {
        if (!wait_for_state(SERVICE_RUNNING, 30000)) return ERROR_TIMEOUT;
        current_state = SERVICE_RUNNING;
    } else if (current_state == SERVICE_PAUSE_PENDING) {
        if (!wait_for_state(SERVICE_PAUSED, 30000)) return ERROR_TIMEOUT;
        current_state = SERVICE_PAUSED;
    }

    if (current_state == target_state) return ERROR_SUCCESS;
    if (current_state == SERVICE_STOPPED) {
        const DWORD start_error = start_service();
        if (start_error != ERROR_SUCCESS && start_error != ERROR_SERVICE_ALREADY_RUNNING) {
            return start_error;
        }
        if (!wait_for_state(SERVICE_RUNNING, 30000)) return ERROR_TIMEOUT;
        current_state = SERVICE_RUNNING;
    }
    if (current_state == SERVICE_PAUSED && target_state == SERVICE_RUNNING) {
        const DWORD continue_error = continue_service();
        if (continue_error != ERROR_SUCCESS) return continue_error;
        return wait_for_state(SERVICE_RUNNING, 30000) ? ERROR_SUCCESS : ERROR_TIMEOUT;
    }
    if (current_state == SERVICE_RUNNING && target_state == SERVICE_PAUSED) {
        const DWORD pause_error = pause_service();
        if (pause_error != ERROR_SUCCESS) return pause_error;
        return wait_for_state(SERVICE_PAUSED, 30000) ? ERROR_SUCCESS : ERROR_TIMEOUT;
    }
    return current_state == target_state ? ERROR_SUCCESS : ERROR_INVALID_STATE;
}

template <typename StopService, typename ClearFirewall, typename RestoreFirewall,
          typename DeleteService, typename RestoreService>
ServiceUninstallResult run_service_uninstall_transaction(
    DWORD previous_state, StopService stop_service, ClearFirewall clear_firewall,
    RestoreFirewall restore_firewall, DeleteService delete_service,
    RestoreService restore_service) {
    ServiceUninstallResult result;
    result.stage = ServiceUninstallStage::Stop;
    RollbackStack rollback;
    if (previous_state != SERVICE_STOPPED && previous_state != SERVICE_STOP_PENDING) {
        rollback.push([restore_service, previous_state] {
            return static_cast<int>(restore_service(previous_state));
        });
    }
    result.error = stop_service();
    if (result.error != ERROR_SUCCESS) {
        result.rollback_error = rollback.rollback();
        return result;
    }

    result.stage = ServiceUninstallStage::Firewall;
    result.error = clear_firewall(&result.firewall_rollback_error);
    if (result.error != ERROR_SUCCESS) {
        result.rollback_error = rollback.rollback();
        return result;
    }
    rollback.push([restore_firewall] { return restore_firewall(); });

    result.stage = ServiceUninstallStage::Delete;
    result.error = delete_service();
    if (result.error != ERROR_SUCCESS) {
        result.rollback_error = rollback.rollback();
        return result;
    }

    rollback.commit();
    result.stage = ServiceUninstallStage::Complete;
    return result;
}

}  // namespace chast::host::detail
