#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "file_snapshot.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

std::wstring temp_file() {
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::wstring(directory, length) + L"chast-file-snapshot-" +
           std::to_wstring(GetCurrentProcessId()) + L".ini";
}

bool write_text(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = text.empty() ||
                    (WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                               nullptr) && written == text.size());
    CloseHandle(file);
    return ok;
}

bool read_text(const std::wstring& path, std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    text.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = text.empty() ||
                    (ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read,
                              nullptr) && read == text.size());
    CloseHandle(file);
    return ok;
}

void test_absent_file() {
    const std::wstring path = temp_file();
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".rollback.tmp").c_str());
    chast::host::detail::FileSnapshot snapshot;
    check(snapshot.capture(path) == ERROR_SUCCESS && !snapshot.existed(),
          "an absent configuration file is snapshotted as absent");
    check(write_text(path, "created=1\n"), "test file is created");
    check(snapshot.restore() == ERROR_SUCCESS &&
              GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
          "rollback removes a file created after the snapshot");
}

void test_existing_file() {
    const std::wstring path = temp_file();
    const std::string original = "token=old\nunknown=value\n";
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".rollback.tmp").c_str());
    check(write_text(path, original), "original configuration is written");
    chast::host::detail::FileSnapshot snapshot;
    check(snapshot.capture(path) == ERROR_SUCCESS && snapshot.existed(),
          "an existing configuration file is snapshotted");
    check(write_text(path, "token=new\n"), "configuration is replaced by test data");
    check(snapshot.restore() == ERROR_SUCCESS, "existing file rollback succeeds");
    std::string restored;
    check(read_text(path, restored) && restored == original,
          "rollback restores the exact original bytes");
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".rollback.tmp").c_str());
}

}  // namespace

int main() {
    test_absent_file();
    test_existing_file();
    return failures == 0 ? 0 : 1;
}
