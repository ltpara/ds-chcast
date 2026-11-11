// Host recording disk guard: recording must stop before the volume fills, and
// the check must resolve a not-yet-created recording directory to the volume
// that will hold it.

#define NOMINMAX

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "chast/host_recording.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

void run() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "chast-recording-disk-test";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (error) {
        std::printf("skip the recording disk guard: %s\n", error.message().c_str());
        return;
    }
    const std::string directory = to_utf8(root.wstring());
    std::string reason;
    check(chast::recording_disk_available(directory, 64ull * 1024 * 1024, &reason),
          "a volume with space passes the guard");
    reason.clear();
    check(!chast::recording_disk_available(directory, UINT64_MAX, &reason) &&
              reason.find("MiB") != std::string::npos && reason.find("reserve") != std::string::npos,
          "a reserve larger than the volume refuses with a size reason");
    reason.clear();
    check(chast::recording_disk_available(directory + "/nested/child", 64ull * 1024 * 1024, &reason),
          "a missing recording directory checks its nearest existing parent");
    reason.clear();
    check(!chast::recording_disk_available("", 64ull * 1024 * 1024, &reason) && !reason.empty(),
          "an empty recording directory is refused");
    reason.clear();
    check(!chast::recording_disk_available("relative/path", 64ull * 1024 * 1024, &reason) &&
              reason.find("absolute") != std::string::npos,
          "a relative recording directory is refused");
    fs::remove_all(root, error);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run();
    return failures ? 1 : 0;
}
