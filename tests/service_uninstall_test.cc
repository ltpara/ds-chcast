#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "service_uninstall.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

struct FakeUninstall {
    std::vector<std::string> events;
    DWORD stop_error = ERROR_SUCCESS;
    DWORD delete_error = ERROR_SUCCESS;
    int clear_error = 0;
    int clear_rollback_error = 0;
    int firewall_restore_error = 0;
    DWORD service_restore_error = ERROR_SUCCESS;
    bool firewall_rules_present = true;
    bool service_deleted = false;

    chast::host::detail::ServiceUninstallResult run(DWORD previous_state) {
        return chast::host::detail::run_service_uninstall_transaction(
            previous_state,
            [this] {
                events.emplace_back("stop");
                return stop_error;
            },
            [this](int* rollback_error) {
                events.emplace_back("clear_firewall");
                *rollback_error = clear_rollback_error;
                if (clear_error == 0) firewall_rules_present = false;
                return clear_error;
            },
            [this] {
                events.emplace_back("restore_firewall");
                firewall_rules_present = true;
                return firewall_restore_error;
            },
            [this] {
                events.emplace_back("delete_service");
                if (delete_error == ERROR_SUCCESS) service_deleted = true;
                return delete_error;
            },
            [this](DWORD state) {
                events.emplace_back(state == SERVICE_PAUSED ? "restore_paused"
                                                            : "restore_running");
                return service_restore_error;
            });
    }
};

void test_stop_failure_attempts_service_state_recovery() {
    FakeUninstall fake;
    fake.stop_error = ERROR_TIMEOUT;
    const auto result = fake.run(SERVICE_RUNNING);
    check(result.stage == chast::host::detail::ServiceUninstallStage::Stop &&
              result.error == ERROR_TIMEOUT && result.rollback_error == 0 &&
              fake.events == std::vector<std::string>{"stop", "restore_running"} &&
              !fake.service_deleted && fake.firewall_rules_present,
          "stop failure restores the service state without clearing firewall or deleting it");

    FakeUninstall restore_failed;
    restore_failed.stop_error = ERROR_TIMEOUT;
    restore_failed.service_restore_error = ERROR_ACCESS_DENIED;
    const auto failed_result = restore_failed.run(SERVICE_RUNNING);
    check(failed_result.error == ERROR_TIMEOUT &&
              failed_result.rollback_error == ERROR_ACCESS_DENIED,
          "stop failure reports a failed attempt to restore the service");
}

void test_firewall_failure_preserves_service() {
    FakeUninstall fake;
    fake.clear_error = ERROR_ACCESS_DENIED;
    fake.clear_rollback_error = ERROR_GEN_FAILURE;
    const auto result = fake.run(SERVICE_RUNNING);
    check(result.stage == chast::host::detail::ServiceUninstallStage::Firewall &&
              result.error == ERROR_ACCESS_DENIED &&
              result.firewall_rollback_error == ERROR_GEN_FAILURE &&
              fake.events == std::vector<std::string>{"stop", "clear_firewall",
                                                       "restore_running"} &&
              !fake.service_deleted && fake.firewall_rules_present,
          "firewall failure prevents deletion and restores the running service");
}

void test_delete_failure_rolls_back_before_restarting() {
    FakeUninstall fake;
    fake.delete_error = ERROR_ACCESS_DENIED;
    const auto result = fake.run(SERVICE_RUNNING);
    check(result.stage == chast::host::detail::ServiceUninstallStage::Delete &&
              result.error == ERROR_ACCESS_DENIED && result.rollback_error == 0 &&
              fake.events == std::vector<std::string>{"stop", "clear_firewall",
                                                       "delete_service", "restore_firewall",
                                                       "restore_running"} &&
              !fake.service_deleted && fake.firewall_rules_present,
          "delete failure restores firewall rules before restarting the service");
}

void test_success_commits_removal() {
    FakeUninstall fake;
    const auto result = fake.run(SERVICE_RUNNING);
    check(result.stage == chast::host::detail::ServiceUninstallStage::Complete &&
              result.error == ERROR_SUCCESS && fake.service_deleted &&
              !fake.firewall_rules_present &&
              fake.events == std::vector<std::string>{"stop", "clear_firewall",
                                                       "delete_service"},
          "successful uninstall commits service and firewall removal");
}

struct FakeRestore {
    DWORD current_state = SERVICE_STOPPED;
    DWORD query_error = ERROR_SUCCESS;
    std::vector<DWORD> waited_states;
    int starts = 0;
    int continues = 0;
    int pauses = 0;
    DWORD start_error = ERROR_SUCCESS;
    DWORD continue_error = ERROR_SUCCESS;
    DWORD pause_error = ERROR_SUCCESS;
    DWORD state_to_fail = 0;

    DWORD run(DWORD previous_state) {
        return chast::host::detail::restore_service_state_after_stop(
            previous_state,
            [this](DWORD* state) {
                if (query_error == ERROR_SUCCESS) *state = current_state;
                return query_error;
            },
            [this] {
                ++starts;
                return start_error;
            },
            [this](DWORD desired, DWORD) {
                waited_states.push_back(desired);
                if (desired == state_to_fail) return false;
                current_state = desired;
                return true;
            },
            [this] {
                ++continues;
                return continue_error;
            },
            [this] {
                ++pauses;
                return pause_error;
            });
    }
};

void test_restore_service_state() {
    FakeRestore running;
    check(running.run(SERVICE_RUNNING) == ERROR_SUCCESS && running.starts == 1 &&
              running.waited_states == std::vector<DWORD>{SERVICE_RUNNING} &&
              running.continues == 0 && running.pauses == 0,
          "rollback starts and waits for a previously running service");

    FakeRestore paused;
    check(paused.run(SERVICE_PAUSED) == ERROR_SUCCESS && paused.starts == 1 &&
              paused.waited_states == std::vector<DWORD>{SERVICE_RUNNING, SERVICE_PAUSED} &&
              paused.continues == 0 && paused.pauses == 1,
          "rollback restores a previously paused service after startup");

    FakeRestore stopping;
    check(stopping.run(SERVICE_STOP_PENDING) == ERROR_SUCCESS && stopping.starts == 0,
          "rollback leaves a service that was stopping in the stopped state");

    FakeRestore stop_timed_out;
    stop_timed_out.current_state = SERVICE_STOP_PENDING;
    check(stop_timed_out.run(SERVICE_RUNNING) == ERROR_SUCCESS &&
              stop_timed_out.waited_states ==
                  std::vector<DWORD>{SERVICE_STOPPED, SERVICE_RUNNING} &&
              stop_timed_out.starts == 1,
          "a timed-out stop is allowed to finish before the prior running state is restored");

    FakeRestore starting;
    starting.current_state = SERVICE_START_PENDING;
    check(starting.run(SERVICE_RUNNING) == ERROR_SUCCESS && starting.starts == 0 &&
              starting.waited_states == std::vector<DWORD>{SERVICE_RUNNING},
          "an in-progress service start is awaited instead of started again");

    FakeRestore continuing;
    continuing.current_state = SERVICE_CONTINUE_PENDING;
    check(continuing.run(SERVICE_RUNNING) == ERROR_SUCCESS && continuing.continues == 0 &&
              continuing.waited_states == std::vector<DWORD>{SERVICE_RUNNING},
          "an in-progress continue is awaited before restoring the prior running state");

    FakeRestore pausing;
    pausing.current_state = SERVICE_PAUSE_PENDING;
    check(pausing.run(SERVICE_PAUSED) == ERROR_SUCCESS && pausing.pauses == 0 &&
              pausing.waited_states == std::vector<DWORD>{SERVICE_PAUSED},
          "an in-progress pause is awaited instead of pausing again");

    FakeRestore pause_before_resume;
    pause_before_resume.current_state = SERVICE_PAUSE_PENDING;
    check(pause_before_resume.run(SERVICE_RUNNING) == ERROR_SUCCESS &&
              pause_before_resume.continues == 1 &&
              pause_before_resume.waited_states ==
                  std::vector<DWORD>{SERVICE_PAUSED, SERVICE_RUNNING},
          "a pending pause settles before resuming a previously running service");

    FakeRestore already_running;
    already_running.current_state = SERVICE_RUNNING;
    check(already_running.run(SERVICE_RUNNING) == ERROR_SUCCESS && already_running.starts == 0 &&
              already_running.waited_states.empty(),
          "rollback does not restart a service that never stopped");

    FakeRestore resume;
    resume.current_state = SERVICE_PAUSED;
    check(resume.run(SERVICE_RUNNING) == ERROR_SUCCESS && resume.continues == 1 &&
              resume.waited_states == std::vector<DWORD>{SERVICE_RUNNING},
          "a paused service is continued when its previous state was running");

    FakeRestore repause;
    repause.current_state = SERVICE_RUNNING;
    check(repause.run(SERVICE_PAUSED) == ERROR_SUCCESS && repause.pauses == 1 &&
              repause.waited_states == std::vector<DWORD>{SERVICE_PAUSED},
          "a running service is paused when its previous state was paused");

    FakeRestore query_failed;
    query_failed.query_error = ERROR_ACCESS_DENIED;
    check(query_failed.run(SERVICE_RUNNING) == ERROR_ACCESS_DENIED &&
              query_failed.starts == 0,
          "a status query failure is reported without changing the service");

    FakeRestore failed_start;
    failed_start.start_error = ERROR_ACCESS_DENIED;
    check(failed_start.run(SERVICE_RUNNING) == ERROR_ACCESS_DENIED,
          "rollback reports a service restart failure");
}

}  // namespace

int main() {
    test_stop_failure_attempts_service_state_recovery();
    test_firewall_failure_preserves_service();
    test_delete_failure_rolls_back_before_restarting();
    test_success_commits_removal();
    test_restore_service_state();
    return failures == 0 ? 0 : 1;
}
