#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "service_install.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

void test_service_configuration_comparison_detects_changes() {
    const std::wstring current = L"\"C:\\apps\\chast-service.exe\" --run";
    const std::wstring other_path = L"\"C:\\next\\chast-service.exe\" --run";
    const std::wstring other_command = L"\"C:\\apps\\chast-service.exe\" --worker";
    check(chast::host::detail::service_configuration_matches_target(
              SERVICE_AUTO_START, current, SERVICE_AUTO_START, current),
          "an identical service path and start type match the target configuration");
    check(!chast::host::detail::service_configuration_matches_target(
              SERVICE_AUTO_START, current, SERVICE_AUTO_START, other_path),
          "a changed service binary path is detected");
    check(!chast::host::detail::service_configuration_matches_target(
              SERVICE_AUTO_START, current, SERVICE_AUTO_START, other_command),
          "a changed service command line is detected");
    check(!chast::host::detail::service_configuration_matches_target(
              SERVICE_DEMAND_START, current, SERVICE_AUTO_START, current),
          "a changed service start type is detected");
}

void test_running_service_is_stopped_for_config_update_and_recovered() {
    std::vector<DWORD> events;
    chast::host::detail::RollbackStack rollback;
    const DWORD result = chast::host::detail::prepare_existing_service_for_update(
        SERVICE_RUNNING, true, false, rollback,
        [&] {
            events.push_back(1);
            return ERROR_SUCCESS;
        },
        [&](DWORD previous_state) {
            events.push_back(previous_state);
            return ERROR_SUCCESS;
        });
    check(result == ERROR_SUCCESS && events == std::vector<DWORD>{1},
          "an active existing service is stopped before its configuration is updated");
    check(rollback.rollback() == 0 &&
              events == std::vector<DWORD>{1, SERVICE_RUNNING},
          "a failed update restores the previously running service");
}

void test_running_idempotent_install_does_not_stop_service() {
    std::vector<DWORD> events;
    chast::host::detail::RollbackStack rollback;
    const DWORD result = chast::host::detail::prepare_existing_service_for_update(
        SERVICE_RUNNING, false, false, rollback,
        [&] {
            events.push_back(1);
            return ERROR_SUCCESS;
        },
        [&](DWORD previous_state) {
            events.push_back(previous_state);
            return ERROR_SUCCESS;
        });
    check(result == ERROR_SUCCESS && events.empty(),
          "an idempotent install leaves a running service uninterrupted");
    check(rollback.rollback() == 0 && events == std::vector<DWORD>{SERVICE_RUNNING},
          "rollback preserves the original running state without stopping it");
}

void test_running_service_stops_for_settings_update() {
    std::vector<DWORD> events;
    chast::host::detail::RollbackStack rollback;
    const DWORD result = chast::host::detail::prepare_existing_service_for_update(
        SERVICE_RUNNING, false, true, rollback,
        [&] {
            events.push_back(1);
            return ERROR_SUCCESS;
        },
        [&](DWORD previous_state) {
            events.push_back(previous_state);
            return ERROR_SUCCESS;
        });
    check(result == ERROR_SUCCESS && events == std::vector<DWORD>{1},
          "a machine settings update stops the running service before writing");
    check(rollback.rollback() == 0 &&
              events == std::vector<DWORD>{1, SERVICE_RUNNING},
          "a failed machine settings update restores the previously running service");
}

void test_stopped_service_needs_no_recovery_action() {
    int stop_calls = 0;
    int restore_calls = 0;
    chast::host::detail::RollbackStack rollback;
    const DWORD result = chast::host::detail::prepare_existing_service_for_update(
        SERVICE_STOPPED, true, true, rollback,
        [&] {
            ++stop_calls;
            return ERROR_SUCCESS;
        },
        [&](DWORD) {
            ++restore_calls;
            return ERROR_SUCCESS;
        });
    check(result == ERROR_SUCCESS && rollback.rollback() == 0 && stop_calls == 0 &&
              restore_calls == 0,
          "a service already stopped stays stopped if the update fails");
}

void test_stop_failure_registers_state_recovery() {
    int restore_calls = 0;
    chast::host::detail::RollbackStack rollback;
    const DWORD result = chast::host::detail::prepare_existing_service_for_update(
        SERVICE_RUNNING, false, true, rollback, [] { return ERROR_TIMEOUT; },
        [&](DWORD) {
            ++restore_calls;
            return ERROR_SUCCESS;
        });
    check(result == ERROR_TIMEOUT && rollback.rollback() == 0 && restore_calls == 1,
          "a failed stop still attempts to restore the original service state");
}

void test_transitional_and_paused_states_are_rejected() {
    const DWORD unsupported_states[] = {SERVICE_START_PENDING, SERVICE_STOP_PENDING,
                                        SERVICE_PAUSED, SERVICE_PAUSE_PENDING,
                                        SERVICE_CONTINUE_PENDING};
    for (const DWORD state : unsupported_states) {
        int stop_calls = 0;
        int restore_calls = 0;
        chast::host::detail::RollbackStack rollback;
        const DWORD result = chast::host::detail::prepare_existing_service_for_update(
            state, true, true, rollback,
            [&] {
                ++stop_calls;
                return ERROR_SUCCESS;
            },
            [&](DWORD) {
                ++restore_calls;
                return ERROR_SUCCESS;
            });
        check(result == ERROR_INVALID_STATE && rollback.rollback() == 0 && stop_calls == 0 &&
                  restore_calls == 0,
              "transitional or paused existing services are rejected before mutation");
    }
}

void test_runtime_configuration_stops_after_first_error_and_rolls_back() {
    std::vector<int> events;
    chast::host::detail::RollbackStack rollback;
    rollback.push([&] {
        events.push_back(4);
        return 0;
    });
    const DWORD result = chast::host::detail::apply_service_runtime_configuration(
        [&] {
            events.push_back(1);
            return ERROR_SUCCESS;
        },
        [&] {
            events.push_back(2);
            return ERROR_ACCESS_DENIED;
        },
        [&] {
            events.push_back(3);
            return ERROR_SUCCESS;
        });
    check(result == ERROR_ACCESS_DENIED && events == std::vector<int>{1, 2},
          "runtime configuration failure stops subsequent SCM mutations");
    check(rollback.rollback() == 0 && events == std::vector<int>{1, 2, 4},
          "runtime configuration failure executes its registered snapshot rollback");
}

void test_runtime_configuration_applies_all_steps() {
    std::vector<int> events;
    const DWORD result = chast::host::detail::apply_service_runtime_configuration(
        [&] {
            events.push_back(1);
            return ERROR_SUCCESS;
        },
        [&] {
            events.push_back(2);
            return ERROR_SUCCESS;
        },
        [&] {
            events.push_back(3);
            return ERROR_SUCCESS;
        });
    check(result == ERROR_SUCCESS && events == std::vector<int>{1, 2, 3},
          "all service runtime configuration steps apply in order");
}

void test_settings_save_failure_is_registered_for_rollback() {
    std::string contents = "original";
    chast::host::detail::RollbackStack rollback;
    const bool saved = chast::host::detail::save_settings_with_rollback(
        rollback,
        [&] {
            contents = "new-but-post-write-validation-failed";
            return false;
        },
        [&] {
            contents = "original";
            return ERROR_SUCCESS;
        });
    check(!saved && contents == "new-but-post-write-validation-failed",
          "a simulated post-write failure reports the save as failed");
    check(rollback.rollback() == ERROR_SUCCESS && contents == "original",
          "a failed settings save restores its snapshot");
}

}  // namespace

int main() {
    test_service_configuration_comparison_detects_changes();
    test_running_service_is_stopped_for_config_update_and_recovered();
    test_running_idempotent_install_does_not_stop_service();
    test_running_service_stops_for_settings_update();
    test_stopped_service_needs_no_recovery_action();
    test_stop_failure_registers_state_recovery();
    test_transitional_and_paused_states_are_rejected();
    test_runtime_configuration_stops_after_first_error_and_rolls_back();
    test_runtime_configuration_applies_all_steps();
    test_settings_save_failure_is_registered_for_rollback();
    return failures == 0 ? 0 : 1;
}
