#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace chast_test {

// Tests can run under a restricted desktop whose inherited %TEMP% is readable
// but not writable. Prefer it when available, then fall back inside the build
// tree. Every caller gets a unique, freshly created directory.
inline std::filesystem::path scratch_directory(const std::string& name) {
    std::error_code ec;
    const std::string unique =
        name + "-" + std::to_string(GetCurrentProcessId());
    const std::filesystem::path temporary = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        const std::filesystem::path candidate = temporary / unique;
        std::filesystem::remove_all(candidate, ec);
        ec.clear();
        std::filesystem::create_directories(candidate, ec);
        if (!ec && std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }

    ec.clear();
    const std::filesystem::path fallback =
        std::filesystem::current_path() / "test-temp" / unique;
    std::filesystem::remove_all(fallback, ec);
    ec.clear();
    std::filesystem::create_directories(fallback, ec);
    if (!ec && std::filesystem::is_directory(fallback)) {
        return fallback;
    }
    return {};
}

}  // namespace chast_test
