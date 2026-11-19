#include <windows.h>

#include <cstdio>
#include <string>

#include "platform.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

std::wstring executable_path() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? std::wstring{} : std::wstring(path, length);
}

std::wstring current_process_file() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::wstring(temp, length) + L"chast-platform-process-" +
           std::to_wstring(GetCurrentProcessId()) + L".pid";
}

void test_normal_exit(const std::wstring& executable) {
    DWORD code = 0;
    const std::wstring command = L"\"" + executable + L"\" --exit-child";
    check(chast::host::run_hidden_command(command, 5000, &code),
          "a child that exits is reported as completed");
    check(code == 23, "the child exit code is preserved");
}

void test_timeout_terminates_child(const std::wstring& executable) {
    const std::wstring pid_file = current_process_file();
    check(!pid_file.empty(), "a temporary child marker path is available");
    if (pid_file.empty()) {
        return;
    }
    DeleteFileW(pid_file.c_str());
    check(SetEnvironmentVariableW(L"CHAST_PLATFORM_PROCESS_TEST_PID", pid_file.c_str()) != FALSE,
          "the child marker path is passed through the environment");

    DWORD code = 0;
    HANDLE child_process = nullptr;
    const std::wstring command = L"\"" + executable + L"\" --sleep-child";
    const ULONGLONG started = GetTickCount64();
    const bool completed = chast::host::run_hidden_command(command, 500, &code, &child_process);
    const DWORD error = GetLastError();
    const ULONGLONG elapsed = GetTickCount64() - started;
    SetEnvironmentVariableW(L"CHAST_PLATFORM_PROCESS_TEST_PID", nullptr);

    check(!completed, "a child exceeding the deadline is reported as failure");
    check(error == ERROR_TIMEOUT, "timeout reports ERROR_TIMEOUT");
    check(elapsed < 5000, "timeout returns promptly after stopping the child");

    HANDLE file = CreateFileW(pid_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD child_pid = 0;
    DWORD read = 0;
    const bool got_pid = file != INVALID_HANDLE_VALUE &&
                         ReadFile(file, &child_pid, sizeof(child_pid), &read, nullptr) != FALSE &&
                         read == sizeof(child_pid);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    check(got_pid, "the child recorded its process id before waiting");
    const bool stopped = child_process != nullptr &&
                         WaitForSingleObject(child_process, 0) == WAIT_OBJECT_0;
    check(stopped, "the timed-out child has exited");
    if (child_process != nullptr) {
        if (!stopped) {
            TerminateProcess(child_process, ERROR_TIMEOUT);
            WaitForSingleObject(child_process, 5000);
        }
        CloseHandle(child_process);
    }
    DeleteFileW(pid_file.c_str());
}

int sleep_child() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"CHAST_PLATFORM_PROCESS_TEST_PID", path,
                                                 MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return 2;
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 3;
    }
    const DWORD pid = GetCurrentProcessId();
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, &pid, sizeof(pid), &written, nullptr);
    CloseHandle(file);
    if (!wrote || written != sizeof(pid)) {
        return 4;
    }
    Sleep(INFINITE);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--exit-child") {
        return 23;
    }
    if (argc > 1 && std::string(argv[1]) == "--sleep-child") {
        return sleep_child();
    }

    const std::wstring executable = executable_path();
    check(!executable.empty(), "the current executable path is available");
    if (!executable.empty()) {
        test_normal_exit(executable);
        test_timeout_terminates_child(executable);
    }
    if (failures != 0) {
        std::printf("%d platform process checks failed\n", failures);
        return 1;
    }
    std::printf("platform process checks passed\n");
    return 0;
}
