#include "chast/wallpaper.h"

#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef SPI_GETDESKTOPWALLPAPER
#define SPI_GETDESKTOPWALLPAPER 0x002B
#endif

namespace chast {

namespace {

constexpr wchar_t kWallpapersKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers";
constexpr wchar_t kWallpaperValue[] = L"BackgroundHistoryPath0";

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size, nullptr, nullptr);
    return out;
}

std::string win32_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length != 0 ? std::wstring(buffer, length)
                                    : L"error " + std::to_wstring(code);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return narrow(text);
}

std::wstring read_registry_wallpaper(HKEY root, const std::wstring& prefix) {
    HKEY key = nullptr;
    const std::wstring path = prefix + kWallpapersKey;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD type = 0;
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LSTATUS status =
        RegQueryValueExW(key, kWallpaperValue, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        return {};
    }
    const size_t length = bytes / sizeof(wchar_t);
    size_t end = 0;
    while (end < length && buffer[end] != L'\0') {
        ++end;
    }
    return std::wstring(buffer.data(), end);
}

// The service runs as SYSTEM inside the user's session, so its HKCU is the
// system profile. Ask the shell's process token for the interactive user SID
// and read that user's wallpaper history instead.
std::wstring shell_user_prefix() {
    const HWND shell = GetShellWindow();
    if (shell == nullptr) {
        return {};
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(shell, &process_id);
    if (process_id == 0) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return {};
    }
    HANDLE token = nullptr;
    const BOOL opened = OpenProcessToken(process, TOKEN_QUERY, &token);
    CloseHandle(process);
    if (!opened || token == nullptr) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> data(size);
    const BOOL read = size != 0 &&
                      GetTokenInformation(token, TokenUser, data.data(), size, &size);
    CloseHandle(token);
    if (!read) {
        return {};
    }
    const PSID sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text) || text == nullptr) {
        return {};
    }
    std::wstring prefix(text);
    LocalFree(text);
    return prefix + L"\\";
}

std::wstring current_wallpaper() {
    std::vector<wchar_t> buffer(32768, L'\0');
    if (SystemParametersInfoW(SPI_GETDESKTOPWALLPAPER,
                              static_cast<UINT>(buffer.size()), buffer.data(), 0)) {
        // An empty answer means a solid-color desktop; falling back to the
        // history here would restore a stale image after the session.
        return std::wstring(buffer.data());
    }
    std::wstring path = read_registry_wallpaper(HKEY_CURRENT_USER, L"");
    if (!path.empty()) {
        return path;
    }
    const std::wstring prefix = shell_user_prefix();
    if (!prefix.empty()) {
        return read_registry_wallpaper(HKEY_USERS, prefix);
    }
    return {};
}

bool set_wallpaper(const std::wstring& path, std::string* error) {
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
                               const_cast<wchar_t*>(path.c_str()),
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        *error = "SystemParametersInfo(SPI_SETDESKWALLPAPER) failed: " +
                 win32_error(GetLastError());
        return false;
    }
    return true;
}

bool read_backup(const std::filesystem::path& path, std::wstring& wallpaper,
                 std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "the wallpaper backup cannot be read";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        wallpaper = widen(line);
        return true;
    }
    return true;
}

bool write_backup(const std::filesystem::path& path, const std::wstring& wallpaper,
                  std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        *error = "the wallpaper backup cannot be written next to the settings file";
        return false;
    }
    out << "# chast wallpaper backup; restored when the session ends or the host restarts\n"
        << narrow(wallpaper) << "\n";
    out.flush();
    if (!out) {
        *error = "the wallpaper backup could not be written";
        return false;
    }
    return true;
}

class Win32WallpaperRemoval final : public WallpaperRemoval {
public:
    Win32WallpaperRemoval(std::filesystem::path state_path, std::wstring wallpaper)
        : state_path_(std::move(state_path)), wallpaper_(std::move(wallpaper)) {}

    ~Win32WallpaperRemoval() override {
        std::string ignored;
        restore(&ignored);
    }

    bool restore(std::string* error) override {
        if (!active_) {
            return true;
        }
        if (!set_wallpaper(wallpaper_, error)) {
            return false;
        }
        std::error_code ignored;
        std::filesystem::remove(state_path_, ignored);
        active_ = false;
        return true;
    }

    bool active() const override { return active_; }

private:
    std::filesystem::path state_path_;
    std::wstring wallpaper_;
    bool active_ = true;
};

}  // namespace

std::unique_ptr<WallpaperRemoval> create_wallpaper_removal(const std::string& state_path,
                                                           std::string* error) {
    if (state_path.empty()) {
        *error = "no wallpaper backup path is configured";
        return nullptr;
    }
    const std::filesystem::path backup = std::filesystem::path(widen(state_path));
    std::error_code exists_error;
    if (std::filesystem::exists(backup, exists_error)) {
        *error = "a wallpaper backup is already pending; the desktop was left as is";
        return nullptr;
    }
    const std::wstring wallpaper = current_wallpaper();
    if (wallpaper.empty()) {
        *error = "the desktop is already a solid color";
        return nullptr;
    }
    if (!write_backup(backup, wallpaper, error)) {
        return nullptr;
    }
    if (!set_wallpaper(L"", error)) {
        std::error_code ignored;
        std::filesystem::remove(backup, ignored);
        return nullptr;
    }
    return std::make_unique<Win32WallpaperRemoval>(backup, wallpaper);
}

bool restore_abandoned_wallpaper(const std::string& state_path, std::string* error) {
    if (state_path.empty()) {
        return true;
    }
    const std::filesystem::path backup = std::filesystem::path(widen(state_path));
    std::error_code exists_error;
    if (!std::filesystem::exists(backup, exists_error)) {
        return true;
    }
    std::wstring wallpaper;
    if (!read_backup(backup, wallpaper, error)) {
        return false;
    }
    if (wallpaper.empty()) {
        std::error_code ignored;
        std::filesystem::remove(backup, ignored);
        return true;
    }
    if (!set_wallpaper(wallpaper, error)) {
        return false;
    }
    std::error_code ignored;
    std::filesystem::remove(backup, ignored);
    return true;
}

}  // namespace chast
