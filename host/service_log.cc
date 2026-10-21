#include "service_log.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "chast/host_settings.h"

namespace chast::host {

namespace {

constexpr uintmax_t kMaxLogBytes = 5u * 1024u * 1024u;

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

void rotate_if_large(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size <= kMaxLogBytes) {
        return;
    }
    const std::filesystem::path old = path.string() + ".old";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(path, old, ec);
}

std::string timestamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u ", local.wYear,
                  local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
    return text;
}

}  // namespace

std::string service_log_path(const char* name) {
    const std::string directory = machine_settings_directory();
    if (directory.empty()) {
        return {};
    }
    return directory + "\\" + name;
}

void log_line(const std::string& path, const std::string& message) {
    if (path.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(log_mutex());
    std::error_code ec;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    rotate_if_large(file);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        return;
    }
    const std::string line = timestamp() + message + "\n";
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace chast::host
