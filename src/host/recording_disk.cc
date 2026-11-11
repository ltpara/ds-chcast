#define NOMINMAX

#include "chast/host_recording.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace chast {
namespace {

namespace fs = std::filesystem;

fs::path from_utf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

}  // namespace

bool recording_disk_available(const std::string& utf8_directory, uint64_t min_free_bytes,
                              std::string* reason) {
    auto refuse = [reason](std::string text) {
        if (reason) *reason = std::move(text);
        return false;
    };
    if (utf8_directory.empty()) {
        return refuse("choose a recording directory before recording");
    }
    fs::path path = from_utf8(utf8_directory);
    if (!path.is_absolute()) {
        return refuse("the recording directory must be absolute");
    }
    std::error_code error;
    while (!path.empty() && !fs::exists(path, error)) {
        const fs::path parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr)) {
        // No free-space report (a UNC share without quota support, a missing
        // drive): refusing here would turn a best-effort guard into a new way
        // for recording to fail, so let the writer report real errors.
        return true;
    }
    if (available.QuadPart >= min_free_bytes) return true;
    char message[192];
    std::snprintf(message, sizeof(message),
                  "the recording volume has %llu MiB free, below the %llu MiB reserve",
                  static_cast<unsigned long long>(available.QuadPart >> 20),
                  static_cast<unsigned long long>(min_free_bytes >> 20));
    return refuse(message);
}

}  // namespace chast
