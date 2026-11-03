#include "chast/virtual_display.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>
#include <softpub.h>
#include <winternl.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace chast {

namespace {

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Hardware ids and the service name are not localized, unlike the device
// description, so they are the only fields used to recognize a driver.
bool has_usbmmidd_marker(const std::string& value) {
    return lower_ascii(value).find("usbmmidd") != std::string::npos;
}

bool has_conflict_marker(const std::string& value) {
    static const char* const markers[] = {
        "mttvdd",
        "virtualdisplaydriver",
        "parsecvdd",
        "rustdeskidd",
    };
    const std::string lower = lower_ascii(value);
    for (const char* marker : markers) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> device_property_strings(HDEVINFO info, SP_DEVINFO_DATA& data,
                                                 DWORD property) {
    DWORD type = 0;
    DWORD bytes = 0;
    SetupDiGetDeviceRegistryPropertyW(info, &data, property, &type, nullptr, 0, &bytes);
    if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(info, &data, property, &type,
                                           reinterpret_cast<PBYTE>(buffer.data()), bytes,
                                           nullptr)) {
        return {};
    }
    std::vector<std::string> values;
    for (const wchar_t* cursor = buffer.data(); *cursor != L'\0';
         cursor += std::wcslen(cursor) + 1) {
        values.push_back(to_utf8(std::wstring(cursor)));
    }
    return values;
}

bool wudf_service_registered() {
    HKEY key = nullptr;
    const LONG result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\usbmmIdd", 0,
        KEY_READ | KEY_WOW64_64KEY, &key);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

bool parent_is_usbmmidd(const std::wstring& parent_id,
                        const std::vector<std::wstring>& adapters) {
    if (std::find(adapters.begin(), adapters.end(), parent_id) != adapters.end()) {
        return true;
    }
    // A root-enumerated device instance id embeds its hardware id, so this
    // also matches when the adapter id could not be read above.
    return has_usbmmidd_marker(to_utf8(parent_id));
}

uint32_t count_usbmmidd_monitors(const std::vector<std::wstring>& adapters) {
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, nullptr, nullptr, DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        return 0;
    }
    uint32_t count = 0;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(info, index, &data); ++index) {
        wchar_t instance[512]{};
        if (!SetupDiGetDeviceInstanceIdW(info, &data, instance,
                                         static_cast<DWORD>(std::size(instance)), nullptr)) {
            continue;
        }
        DEVINST node = 0;
        if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            continue;
        }
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) {
            continue;
        }
        wchar_t parent_id[512]{};
        if (CM_Get_Device_IDW(parent, parent_id, static_cast<ULONG>(std::size(parent_id)), 0) !=
            CR_SUCCESS) {
            continue;
        }
        if (parent_is_usbmmidd(parent_id, adapters)) {
            ++count;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return count;
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string trim_ascii(std::string text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

bool read_file_bytes(const std::wstring& path, std::string* out, std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "cannot open " + to_utf8(path) + " (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 512ll * 1024 * 1024) {
        CloseHandle(file);
        if (error != nullptr) {
            *error = to_utf8(path) + " has an unusable size";
        }
        return false;
    }
    out->assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < out->size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<size_t>(out->size() - offset, static_cast<size_t>(1) << 20));
        if (!ReadFile(file, out->data() + offset, want, &read, nullptr)) {
            CloseHandle(file);
            if (error != nullptr) {
                *error = "cannot read " + to_utf8(path) + " (error " +
                         std::to_string(GetLastError()) + ")";
            }
            return false;
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }
    CloseHandle(file);
    out->resize(offset);
    return true;
}

std::string hex_bytes(const unsigned char* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string hex(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        hex[i * 2] = digits[data[i] >> 4];
        hex[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return hex;
}

bool sha256_bytes_hex(const void* data, size_t size, std::string* hex, std::string* error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        if (error != nullptr) {
            *error = "SHA-256 is unavailable";
        }
        return false;
    }
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD written = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                &written, 0) >= 0 &&
              BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
                                &written, 0) >= 0;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object(object_size);
    std::vector<unsigned char> digest(hash_size);
    if (ok) {
        ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
    }
    if (ok) {
        ok = BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const UCHAR*>(data)),
                            static_cast<ULONG>(size), 0) >= 0 &&
             BCryptFinishHash(hash, digest.data(), hash_size, 0) >= 0;
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) {
        if (error != nullptr) {
            *error = "SHA-256 could not be computed";
        }
        return false;
    }
    *hex = hex_bytes(digest.data(), digest.size());
    return true;
}

bool normalize_manifest_path(const std::string& input, std::string* out, std::string* error) {
    std::string path = trim_ascii(input);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty()) {
        if (error != nullptr) {
            *error = "the manifest contains an empty path";
        }
        return false;
    }
    if (path.front() == '/' || path.find(':') != std::string::npos) {
        if (error != nullptr) {
            *error = "the manifest path " + path + " is absolute";
        }
        return false;
    }
    std::string normalized;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") {
            if (error != nullptr) {
                *error = "the manifest path " + path + " escapes the package";
            }
            return false;
        }
        if (!normalized.empty()) {
            normalized.push_back('/');
        }
        normalized += part;
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    *out = normalized;
    return true;
}

bool is_hex_sha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool run_process(const std::wstring& exe, const std::vector<std::wstring>& args,
                 const std::wstring& working_dir, DWORD timeout_ms, DWORD* exit_code,
                 std::string* error) {
    std::wstring command = L"\"" + exe + L"\"";
    for (const std::wstring& arg : args) {
        command += L" \"" + arg + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, working_dir.empty() ? nullptr : working_dir.c_str(), &startup,
                        &process)) {
        if (error != nullptr) {
            *error = "cannot run " + to_utf8(exe) + " (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (error != nullptr) {
            *error = to_utf8(exe) + " did not finish within " +
                     std::to_string(timeout_ms / 1000) + " s";
        }
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    *exit_code = code;
    return true;
}

bool wait_for_probe(const std::function<bool(const VirtualDisplayProbe&)>& predicate,
                    DWORD timeout_ms, std::string* error) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (predicate(probe_virtual_display())) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(250);
    }
    if (error != nullptr) {
        *error = "timed out waiting for the virtual display to change";
    }
    return false;
}

bool process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

bool has_virtual_display_marker(const std::wstring& value) {
    const std::string text = lower_ascii(to_utf8(value));
    return text.find("usbmmid") != std::string::npos ||
           text.find("usb mobile monitor") != std::string::npos;
}

std::wstring find_usbmmidd_display() {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        if (has_virtual_display_marker(device.DeviceString) ||
            has_virtual_display_marker(device.DeviceID)) {
            return device.DeviceName;
        }
        device.cb = sizeof(device);
    }
    return {};
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring program_data_dir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD size = GetEnvironmentVariableW(L"ProgramData", buffer,
                                               static_cast<DWORD>(std::size(buffer)));
    if (size == 0 || size >= std::size(buffer)) {
        return L"C:\\ProgramData";
    }
    return std::wstring(buffer, size);
}

std::wstring virtual_display_state_path() {
    return program_data_dir() + L"\\chast\\virtual-display.json";
}

std::wstring virtual_display_log_path() {
    return program_data_dir() + L"\\chast\\vdisplay.log";
}

void append_log(const std::wstring& path, const std::string& line) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string text = virtual_display_timestamp_utc() + " " + line + "\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

bool write_text_file(const std::wstring& path, const std::string& text, std::string* error) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    }
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "cannot create " + to_utf8(temporary) + " (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                              nullptr) &&
                    written == text.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        if (error != nullptr) {
            *error = "cannot write " + to_utf8(path);
        }
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        if (error != nullptr) {
            *error = "cannot replace " + to_utf8(path) + " (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    return true;
}

bool read_text_file(const std::wstring& path, std::string* text) {
    std::string error;
    return read_file_bytes(path, text, &error);
}

VirtualDisplayPackageStatus verify_package_files(
    const std::wstring& package_dir, std::string* detail, std::string* package_version,
    std::string* package_sha256) {
    const std::vector<std::string> required = {
        "deviceinstaller64.exe", "usbmmidd.inf",    "usbmmidd.cat", "x64/usbmmidd.dll",
        "win32/usbmmidd.dll",    "license.txt",     "manifest.sha256",
    };
    for (const std::string& name : required) {
        if (!file_exists(package_dir + L"\\" + utf8_to_wide(name))) {
            if (detail != nullptr) {
                *detail = "the driver package is missing " + name;
            }
            return VirtualDisplayPackageStatus::Missing;
        }
    }
    std::string manifest_text;
    std::string error;
    if (!read_file_bytes(package_dir + L"\\manifest.sha256", &manifest_text, &error)) {
        if (detail != nullptr) {
            *detail = error;
        }
        return VirtualDisplayPackageStatus::Missing;
    }
    std::vector<VirtualDisplayManifestEntry> entries;
    if (!parse_virtual_display_manifest(manifest_text, &entries, &error)) {
        if (detail != nullptr) {
            *detail = "manifest.sha256: " + error;
        }
        return VirtualDisplayPackageStatus::Missing;
    }
    std::set<std::string> listed;
    for (const VirtualDisplayManifestEntry& entry : entries) {
        listed.insert(lower_ascii(entry.path));
    }
    for (const std::string& name : required) {
        if (name == "manifest.sha256") {
            continue;
        }
        if (listed.find(lower_ascii(name)) == listed.end()) {
            if (detail != nullptr) {
                *detail = "manifest.sha256 does not list " + name;
            }
            return VirtualDisplayPackageStatus::Missing;
        }
    }
    for (const VirtualDisplayManifestEntry& entry : entries) {
        const std::wstring path = package_dir + L"\\" + utf8_to_wide(entry.path);
        std::string digest;
        if (!sha256_file_hex(path, &digest, &error)) {
            if (detail != nullptr) {
                *detail = error;
            }
            return VirtualDisplayPackageStatus::Missing;
        }
        if (lower_ascii(digest) != lower_ascii(entry.sha256)) {
            if (detail != nullptr) {
                *detail = entry.path + " does not match its manifest hash";
            }
            return VirtualDisplayPackageStatus::HashMismatch;
        }
    }
    std::string source_text;
    if (read_text_file(package_dir + L"\\SOURCE.txt", &source_text)) {
        size_t start = 0;
        while (start <= source_text.size()) {
            const size_t end = source_text.find('\n', start);
            std::string line = source_text.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string key = lower_ascii(trim_ascii(line.substr(0, colon)));
                const std::string value = trim_ascii(line.substr(colon + 1));
                if (key == "version" && package_version != nullptr) {
                    *package_version = value;
                } else if (key == "sha256" && package_sha256 != nullptr &&
                           is_hex_sha256(value)) {
                    *package_sha256 = lower_ascii(value);
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    if (package_sha256 != nullptr && package_sha256->empty()) {
        std::string digest;
        if (sha256_bytes_hex(manifest_text.data(), manifest_text.size(), &digest, &error)) {
            *package_sha256 = digest;
        }
    }
    if (!verify_authenticode(package_dir + L"\\deviceinstaller64.exe", &error)) {
        if (detail != nullptr) {
            *detail = "deviceinstaller64.exe is not trusted: " + error;
        }
        return VirtualDisplayPackageStatus::SignatureMismatch;
    }
    if (detail != nullptr) {
        *detail = "driver package verified (" + std::to_string(entries.size()) + " files)";
    }
    return VirtualDisplayPackageStatus::Ok;
}

class UsbmmiddProvider final : public VirtualDisplayProvider {
  public:
    explicit UsbmmiddProvider(std::wstring package_dir)
        : package_dir_(std::move(package_dir)) {}

    VirtualDisplayProviderKind kind() const override {
        return VirtualDisplayProviderKind::Usbmmidd;
    }

    VirtualDisplayStatus status() const override {
        return classify_virtual_display_probe(probe_virtual_display());
    }

    std::vector<VirtualDisplayMode> modes() const override {
        std::vector<VirtualDisplayMode> live;
        const std::wstring device = find_usbmmidd_display();
        if (!device.empty()) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            std::set<std::array<uint32_t, 3>> seen;
            for (DWORD index = 0; EnumDisplaySettingsW(device.c_str(), index, &mode); ++index) {
                if (mode.dmPelsWidth < 640 || mode.dmPelsHeight < 480) {
                    continue;
                }
                if (seen.insert({mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency})
                        .second) {
                    live.push_back({mode.dmPelsWidth, mode.dmPelsHeight,
                                    mode.dmDisplayFrequency});
                }
            }
        }
        if (live.empty()) {
            return usbmmidd_default_modes();
        }
        std::vector<VirtualDisplayMode> documented;
        for (const VirtualDisplayMode& mode : live) {
            for (const VirtualDisplayMode& known : usbmmidd_default_modes()) {
                if (mode.width == known.width && mode.height == known.height) {
                    documented.push_back(mode);
                    break;
                }
            }
        }
        return documented.empty() ? usbmmidd_default_modes() : documented;
    }

    bool install(std::string* error) override {
        std::string detail;
        const VirtualDisplayPackageStatus check =
            verify_package_files(package_dir_, &detail, nullptr, nullptr);
        if (check != VirtualDisplayPackageStatus::Ok) {
            if (error != nullptr) {
                *error = detail;
            }
            return false;
        }
        const VirtualDisplayProbe probe = probe_virtual_display();
        if (probe.adapter_present || probe.service_registered) {
            return true;
        }
        DWORD code = 1;
        if (!run_process(installer_path(),
                         {L"install", L"usbmmidd.inf", L"usbmmidd"}, package_dir_, 300000,
                         &code, error)) {
            return false;
        }
        if (code != 0) {
            if (error != nullptr) {
                *error = "deviceinstaller64 install exited with code " +
                         std::to_string(code);
            }
            return false;
        }
        return wait_for_probe(
            [](const VirtualDisplayProbe& current) {
                return current.adapter_present || current.service_registered;
            },
            60000, error);
    }

    bool uninstall(std::string* error) override {
        const VirtualDisplayProbe probe = probe_virtual_display();
        if (!probe.adapter_present && !probe.service_registered) {
            return true;
        }
        if (package_dir_.empty() || !file_exists(installer_path())) {
            if (error != nullptr) {
                *error = "the usbmmidd package directory is missing; cannot uninstall the driver";
            }
            return false;
        }
        DWORD code = 1;
        std::string ignored;
        (void)run_process(installer_path(), {L"stop", L"usbmmidd"}, package_dir_, 120000, &code,
                          &ignored);
        if (!run_process(installer_path(), {L"remove", L"usbmmidd"}, package_dir_, 300000, &code,
                         error)) {
            return false;
        }
        if (code != 0) {
            if (error != nullptr) {
                *error = "deviceinstaller64 remove exited with code " + std::to_string(code);
            }
            return false;
        }
        return wait_for_probe(
            [](const VirtualDisplayProbe& current) {
                return !current.adapter_present && !current.service_registered;
            },
            60000, error);
    }

    bool set_monitor_count(uint32_t count, std::string* error) override {
        if (count > kMaxVirtualDisplayMonitors) {
            if (error != nullptr) {
                *error = "usbmmidd supports at most " +
                         std::to_string(kMaxVirtualDisplayMonitors) + " virtual monitors";
            }
            return false;
        }
        const VirtualDisplayProbe probe = probe_virtual_display();
        if (!probe.adapter_present && !probe.service_registered) {
            if (error != nullptr) {
                *error = "usbmmidd is not installed";
            }
            return false;
        }
        if (probe.active_monitors == count) {
            return true;
        }
        if (package_dir_.empty() || !file_exists(installer_path())) {
            if (error != nullptr) {
                *error = "the usbmmidd package directory is required to change the monitor count";
            }
            return false;
        }
        uint32_t current = std::min(probe.active_monitors, kMaxVirtualDisplayMonitors);
        while (current < count) {
            DWORD code = 1;
            if (!run_process(installer_path(), {L"enableidd", L"1"}, package_dir_, 120000, &code,
                             error)) {
                return false;
            }
            if (code != 0) {
                if (error != nullptr) {
                    *error = "deviceinstaller64 enableidd exited with code " +
                             std::to_string(code);
                }
                return false;
            }
            const uint32_t before = current;
            if (!wait_for_probe(
                    [before](const VirtualDisplayProbe& value) {
                        return value.active_monitors > before;
                    },
                    30000, error)) {
                return false;
            }
            current = std::min(probe_virtual_display().active_monitors,
                               kMaxVirtualDisplayMonitors);
        }
        while (current > count) {
            DWORD code = 1;
            if (!run_process(installer_path(), {L"enableidd", L"0"}, package_dir_, 120000, &code,
                             error)) {
                return false;
            }
            if (code != 0) {
                if (error != nullptr) {
                    *error = "deviceinstaller64 enableidd exited with code " +
                             std::to_string(code);
                }
                return false;
            }
            const uint32_t before = current;
            if (!wait_for_probe(
                    [before](const VirtualDisplayProbe& value) {
                        return value.active_monitors < before;
                    },
                    30000, error)) {
                return false;
            }
            current = probe_virtual_display().active_monitors;
        }
        return true;
    }

    bool select_mode(const VirtualDisplayMode& mode, std::string* error) override {
        if (mode.width == 0 || mode.height == 0) {
            if (error != nullptr) {
                *error = "the requested virtual display mode is empty";
            }
            return false;
        }
        const std::wstring device = find_usbmmidd_display();
        if (device.empty()) {
            if (error != nullptr) {
                *error = "no usbmmidd display is active";
            }
            return false;
        }
        DEVMODEW supported{};
        supported.dmSize = sizeof(supported);
        bool found = false;
        for (DWORD index = 0; EnumDisplaySettingsW(device.c_str(), index, &supported); ++index) {
            if (supported.dmPelsWidth == mode.width &&
                supported.dmPelsHeight == mode.height &&
                (mode.refresh_hz == 0 || supported.dmDisplayFrequency == mode.refresh_hz)) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (error != nullptr) {
                *error = "the virtual display does not offer " + std::to_string(mode.width) +
                         "x" + std::to_string(mode.height);
            }
            return false;
        }
        DEVMODEW target{};
        target.dmSize = sizeof(target);
        target.dmPelsWidth = mode.width;
        target.dmPelsHeight = mode.height;
        target.dmBitsPerPel = 32;
        target.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        if (mode.refresh_hz != 0) {
            target.dmDisplayFrequency = mode.refresh_hz;
            target.dmFields |= DM_DISPLAYFREQUENCY;
        }
        const LONG change =
            ChangeDisplaySettingsExW(device.c_str(), &target, nullptr, CDS_UPDATEREGISTRY,
                                     nullptr);
        if (change != DISP_CHANGE_SUCCESSFUL) {
            if (error != nullptr) {
                *error = "ChangeDisplaySettingsEx failed (code " +
                         std::to_string(change) + ")";
            }
            return false;
        }
        return true;
    }

  private:
    std::wstring installer_path() const {
        return package_dir_ + L"\\deviceinstaller64.exe";
    }

    std::wstring package_dir_;
};

}  // namespace

VirtualDisplayStatus classify_virtual_display_probe(const VirtualDisplayProbe& probe) {
    VirtualDisplayStatus status;
    if (!probe.platform_supported) {
        status.state = VirtualDisplayState::Unsupported;
        status.detail = probe.platform_reason.empty()
                            ? "virtual displays are not supported on this platform"
                            : probe.platform_reason;
        return status;
    }
    if (!probe.scan_error.empty()) {
        status.state = VirtualDisplayState::Error;
        status.detail = probe.scan_error;
        return status;
    }
    if (!probe.conflicting_driver.empty()) {
        status.state = VirtualDisplayState::Error;
        status.detail = "another virtual display driver is present: " + probe.conflicting_driver;
        return status;
    }
    status.provider = VirtualDisplayProviderKind::Usbmmidd;
    status.max_monitors = kMaxVirtualDisplayMonitors;
    status.active_monitors = std::min(probe.active_monitors, kMaxVirtualDisplayMonitors);
    if (probe.active_monitors > 0) {
        status.state = VirtualDisplayState::Active;
        status.detail = "usbmmidd is serving " + std::to_string(status.active_monitors) +
                        " virtual monitor(s)";
    } else if (probe.adapter_present || probe.service_registered) {
        status.state = VirtualDisplayState::Installed;
        status.detail = "usbmmidd is installed but no virtual monitor is active";
    } else {
        status.state = VirtualDisplayState::NotInstalled;
        status.detail = "usbmmidd is not installed";
    }
    return status;
}

VirtualDisplayProbe probe_virtual_display() {
    VirtualDisplayProbe probe;
    probe.platform_supported = virtual_display_platform_supported(&probe.platform_reason);
    if (!probe.platform_supported) {
        return probe;
    }
    probe.service_registered = wudf_service_registered();
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                         DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        probe.scan_error = "display devices could not be enumerated";
        return probe;
    }
    std::vector<std::wstring> adapters;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(info, index, &data); ++index) {
        bool usbmmidd = false;
        for (const std::string& id : device_property_strings(info, data, SPDRP_HARDWAREID)) {
            if (has_usbmmidd_marker(id)) {
                usbmmidd = true;
            } else if (probe.conflicting_driver.empty() && has_conflict_marker(id)) {
                probe.conflicting_driver = id;
            }
        }
        for (const std::string& service : device_property_strings(info, data, SPDRP_SERVICE)) {
            if (has_usbmmidd_marker(service)) {
                usbmmidd = true;
            } else if (probe.conflicting_driver.empty() && has_conflict_marker(service)) {
                probe.conflicting_driver = service;
            }
        }
        if (!usbmmidd) {
            continue;
        }
        probe.adapter_present = true;
        wchar_t instance[512]{};
        if (SetupDiGetDeviceInstanceIdW(info, &data, instance,
                                        static_cast<DWORD>(std::size(instance)), nullptr)) {
            adapters.emplace_back(instance);
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    if (probe.adapter_present) {
        probe.active_monitors = count_usbmmidd_monitors(adapters);
    }
    return probe;
}

VirtualDisplayProviderKind detect_virtual_display_provider() {
    const VirtualDisplayProbe probe = probe_virtual_display();
    if (!probe.platform_supported || !probe.conflicting_driver.empty()) {
        return VirtualDisplayProviderKind::None;
    }
    return (probe.adapter_present || probe.service_registered)
               ? VirtualDisplayProviderKind::Usbmmidd
               : VirtualDisplayProviderKind::None;
}

VirtualDisplayMode select_virtual_display_mode(const std::vector<VirtualDisplayMode>& modes,
                                               uint32_t width, uint32_t height) {
    if (modes.empty()) {
        return {1920, 1080, 60};
    }
    if (width == 0 || height == 0) {
        return modes.front();
    }
    const VirtualDisplayMode* exact = nullptr;
    for (const VirtualDisplayMode& mode : modes) {
        if (mode.width == width && mode.height == height &&
            (exact == nullptr || mode.refresh_hz > exact->refresh_hz)) {
            exact = &mode;
        }
    }
    if (exact != nullptr) {
        return *exact;
    }
    const double requested_ratio = static_cast<double>(width) / static_cast<double>(height);
    const uint64_t requested_area = static_cast<uint64_t>(width) * height;
    const VirtualDisplayMode* best = nullptr;
    double best_ratio_delta = std::numeric_limits<double>::max();
    uint64_t best_area_delta = std::numeric_limits<uint64_t>::max();
    for (const VirtualDisplayMode& mode : modes) {
        if (mode.width == 0 || mode.height == 0) {
            continue;
        }
        const double ratio_delta =
            std::abs(static_cast<double>(mode.width) / static_cast<double>(mode.height) -
                     requested_ratio);
        const uint64_t area = static_cast<uint64_t>(mode.width) * mode.height;
        const uint64_t area_delta = area > requested_area ? area - requested_area
                                                          : requested_area - area;
        if (best == nullptr || ratio_delta < best_ratio_delta ||
            (ratio_delta == best_ratio_delta &&
             (area_delta < best_area_delta ||
              (area_delta == best_area_delta &&
               (mode.width < best->width ||
                (mode.width == best->width && mode.height < best->height)))))) {
            best = &mode;
            best_ratio_delta = ratio_delta;
            best_area_delta = area_delta;
        }
    }
    return best != nullptr ? *best : modes.front();
}

const std::vector<VirtualDisplayMode>& usbmmidd_default_modes() {
    static const std::vector<VirtualDisplayMode> modes = {
        {800, 600, 60},   {1024, 768, 60},  {1280, 720, 60},  {1280, 800, 60},
        {1280, 1024, 60}, {1360, 768, 60},  {1440, 900, 60},  {1600, 900, 60},
        {1680, 1050, 60}, {1920, 1080, 60}, {1920, 1200, 60}, {2048, 1152, 60},
        {2560, 1440, 60}, {2560, 1600, 60}, {3840, 2160, 60},
    };
    return modes;
}

const char* virtual_display_provider_name(VirtualDisplayProviderKind kind) {
    switch (kind) {
        case VirtualDisplayProviderKind::None:
            return "none";
        case VirtualDisplayProviderKind::Usbmmidd:
            return "usbmmidd";
        case VirtualDisplayProviderKind::Vdd:
            return "vdd";
    }
    return "none";
}

const char* virtual_display_state_name(VirtualDisplayState state) {
    switch (state) {
        case VirtualDisplayState::Unsupported:
            return "unsupported";
        case VirtualDisplayState::NotInstalled:
            return "not_installed";
        case VirtualDisplayState::Installed:
            return "installed";
        case VirtualDisplayState::Active:
            return "active";
        case VirtualDisplayState::Error:
            return "error";
    }
    return "unsupported";
}

VirtualDisplayPlan plan_virtual_display(VirtualDisplayPolicy policy,
                                        uint32_t requested_monitors,
                                        const std::string& requested_resolution,
                                        const std::vector<VirtualDisplayMode>& modes,
                                        const VirtualDisplayStatus& status,
                                        bool active_display_present) {
    VirtualDisplayPlan plan;
    if (policy == VirtualDisplayPolicy::Off) {
        plan.reason = "virtual display management is off";
        return plan;
    }
    if (status.state == VirtualDisplayState::Unsupported) {
        plan.action = VirtualDisplayAction::Skip;
        plan.reason = "virtual display is not supported: " + status.detail;
        return plan;
    }
    if (status.state == VirtualDisplayState::Error) {
        plan.action = VirtualDisplayAction::Skip;
        plan.reason = "virtual display cannot be managed: " + status.detail;
        return plan;
    }
    if (status.state == VirtualDisplayState::NotInstalled) {
        plan.action = VirtualDisplayAction::Skip;
        plan.reason = "the usbmmidd driver is not installed; run chast-setup with "
                      "--virtual-display to install it";
        return plan;
    }
    if (policy == VirtualDisplayPolicy::Auto && active_display_present) {
        plan.action = VirtualDisplayAction::Skip;
        plan.reason = "a display is already active; leaving the topology unchanged";
        return plan;
    }
    plan.action = VirtualDisplayAction::Ensure;
    plan.target_monitors =
        requested_monitors == 0 ? 1 : std::min(requested_monitors, kMaxVirtualDisplayMonitors);
    plan.reason = "ensure " + std::to_string(plan.target_monitors) + " virtual monitor(s)";
    std::string normalized;
    if (!parse_virtual_display_resolution(requested_resolution, normalized)) {
        return plan;
    }
    const size_t separator = normalized.find('x');
    uint32_t width = 0;
    uint32_t height = 0;
    const char* begin = normalized.data();
    const char* end = begin + normalized.size();
    const auto width_result = std::from_chars(begin, begin + separator, width);
    const auto height_result = std::from_chars(begin + separator + 1, end, height);
    if (width_result.ec != std::errc() || width_result.ptr != begin + separator ||
        height_result.ec != std::errc() || height_result.ptr != end) {
        return plan;
    }
    plan.apply_mode = true;
    plan.mode = select_virtual_display_mode(modes, width, height);
    plan.reason += " at " + std::to_string(plan.mode.width) + "x" +
                   std::to_string(plan.mode.height);
    return plan;
}

VirtualDisplayPrepareResult prepare_virtual_display(
    VirtualDisplayPolicy policy, uint32_t requested_monitors,
    const std::string& requested_resolution, VirtualDisplayProvider* provider,
    bool active_display_present) {
    VirtualDisplayPrepareResult result;
    if (policy == VirtualDisplayPolicy::Off) {
        result.detail = "virtual display management is off";
        return result;
    }
    if (provider == nullptr) {
        result.detail = "no virtual display provider is available";
        return result;
    }
    result.status = provider->status();
    result.plan = plan_virtual_display(policy, requested_monitors, requested_resolution,
                                       provider->modes(), result.status, active_display_present);
    if (result.plan.action != VirtualDisplayAction::Ensure) {
        result.detail = result.plan.reason;
        return result;
    }
    const uint32_t target = result.plan.target_monitors;
    std::string error;
    if (result.status.active_monitors < target) {
        if (!provider->set_monitor_count(target, &error)) {
            result.status = provider->status();
            result.detail = "cannot enable virtual displays: " + error;
            return result;
        }
        result.monitors_applied = true;
    }
    result.status = provider->status();
    if (result.plan.apply_mode && result.status.active_monitors > 0) {
        if (!provider->select_mode(result.plan.mode, &error)) {
            result.detail = "virtual display active with " +
                            std::to_string(result.status.active_monitors) +
                            " monitor(s), but the preferred mode was not applied: " + error;
            return result;
        }
        result.mode_applied = true;
    }
    if (result.status.active_monitors == 0) {
        result.detail = "the provider reported no active virtual monitor after enabling";
        return result;
    }
    result.detail =
        "virtual display ready: " + std::to_string(result.status.active_monitors) + " monitor(s)";
    if (result.plan.apply_mode && result.mode_applied) {
        result.detail += " at " + std::to_string(result.plan.mode.width) + "x" +
                         std::to_string(result.plan.mode.height);
    }
    return result;
}

std::unique_ptr<VirtualDisplayProvider> make_virtual_display_provider(
    VirtualDisplayProviderKind kind, const std::wstring& package_dir) {
    if (kind != VirtualDisplayProviderKind::Usbmmidd) {
        return nullptr;
    }
    return std::make_unique<UsbmmiddProvider>(package_dir);
}

std::wstring default_virtual_display_package_dir() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    std::wstring path(buffer, buffer + size);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash);
    }
    return path + L"\\drivers\\usbmmidd_v2";
}

bool sha256_file_hex(const std::wstring& path, std::string* hex, std::string* error) {
    std::string bytes;
    if (!read_file_bytes(path, &bytes, error)) {
        return false;
    }
    return sha256_bytes_hex(bytes.data(), bytes.size(), hex, error);
}

bool parse_virtual_display_manifest(const std::string& text,
                                    std::vector<VirtualDisplayManifestEntry>* entries,
                                    std::string* error) {
    if (entries == nullptr) {
        if (error != nullptr) {
            *error = "no manifest output";
        }
        return false;
    }
    entries->clear();
    std::set<std::string> seen;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim_ascii(line);
        if (!line.empty() && line.front() != '#') {
            const size_t space = line.find_first_of(" \t");
            if (space == std::string::npos) {
                if (error != nullptr) {
                    *error = "line '" + line + "' has no path";
                }
                return false;
            }
            const std::string digest = lower_ascii(trim_ascii(line.substr(0, space)));
            if (!is_hex_sha256(digest)) {
                if (error != nullptr) {
                    *error = "line '" + line + "' does not start with a SHA-256";
                }
                return false;
            }
            std::string path = trim_ascii(line.substr(space));
            if (!path.empty() && path.front() == '*') {
                path = trim_ascii(path.substr(1));
            }
            std::string normalized;
            if (!normalize_manifest_path(path, &normalized, error)) {
                return false;
            }
            if (!seen.insert(lower_ascii(normalized)).second) {
                if (error != nullptr) {
                    *error = "duplicate manifest path " + normalized;
                }
                return false;
            }
            entries->push_back({digest, normalized});
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (entries->empty()) {
        if (error != nullptr) {
            *error = "the manifest lists no files";
        }
        return false;
    }
    return true;
}

bool verify_authenticode(const std::wstring& path, std::string* error) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG status = WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    if (status != ERROR_SUCCESS) {
        if (error != nullptr) {
            char code[16]{};
            std::snprintf(code, sizeof(code), "0x%08lx",
                          static_cast<unsigned long>(status));
            *error = "the Authenticode signature is not trusted (" + std::string(code) + ")";
        }
        return false;
    }
    return true;
}

VirtualDisplayPackageStatus verify_virtual_display_package(
    const std::wstring& package_dir, std::string* detail, std::string* package_version,
    std::string* package_sha256) {
    if (package_version != nullptr) {
        package_version->clear();
    }
    if (package_sha256 != nullptr) {
        package_sha256->clear();
    }
    std::string reason;
    if (!virtual_display_platform_supported(&reason)) {
        if (detail != nullptr) {
            *detail = reason;
        }
        return VirtualDisplayPackageStatus::Unsupported;
    }
    if (package_dir.empty()) {
        if (detail != nullptr) {
            *detail = "no driver package directory was given";
        }
        return VirtualDisplayPackageStatus::Missing;
    }
    return verify_package_files(package_dir, detail, package_version, package_sha256);
}

namespace {

std::string json_escape(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char escaped[8]{};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

bool json_find(const std::string& text, const std::string& key, size_t* value) {
    const std::string quoted = "\"" + key + "\"";
    size_t position = text.find(quoted);
    if (position == std::string::npos) {
        return false;
    }
    position += quoted.size();
    position = text.find(':', position);
    if (position == std::string::npos) {
        return false;
    }
    ++position;
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
    *value = position;
    return true;
}

bool json_parse_string(const std::string& text, size_t position, std::string* out,
                       std::string* error) {
    if (position >= text.size() || text[position] != '"') {
        if (error != nullptr) {
            *error = "expected a JSON string";
        }
        return false;
    }
    ++position;
    out->clear();
    while (position < text.size()) {
        const char c = text[position++];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            out->push_back(c);
            continue;
        }
        if (position >= text.size()) {
            break;
        }
        const char escape = text[position++];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                out->push_back(escape);
                break;
            case 'b':
                out->push_back('\b');
                break;
            case 'f':
                out->push_back('\f');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'u': {
                if (position + 4 > text.size()) {
                    if (error != nullptr) {
                        *error = "truncated JSON escape";
                    }
                    return false;
                }
                unsigned codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = text[position++];
                    codepoint <<= 4;
                    if (digit >= '0' && digit <= '9') {
                        codepoint |= static_cast<unsigned>(digit - '0');
                    } else if (digit >= 'a' && digit <= 'f') {
                        codepoint |= static_cast<unsigned>(digit - 'a' + 10);
                    } else if (digit >= 'A' && digit <= 'F') {
                        codepoint |= static_cast<unsigned>(digit - 'A' + 10);
                    } else {
                        if (error != nullptr) {
                            *error = "invalid JSON escape";
                        }
                        return false;
                    }
                }
                if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
                    if (error != nullptr) {
                        *error = "surrogate escapes are not supported";
                    }
                    return false;
                }
                if (codepoint < 0x80) {
                    out->push_back(static_cast<char>(codepoint));
                } else if (codepoint < 0x800) {
                    out->push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
                    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
                } else {
                    out->push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
                    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
                    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
                }
                break;
            }
            default:
                if (error != nullptr) {
                    *error = "invalid JSON escape";
                }
                return false;
        }
    }
    if (error != nullptr) {
        *error = "unterminated JSON string";
    }
    return false;
}

bool json_string_field(const std::string& text, const std::string& key, std::string* out,
                       std::string* error) {
    size_t position = 0;
    if (!json_find(text, key, &position)) {
        if (error != nullptr) {
            *error = "missing " + key;
        }
        return false;
    }
    return json_parse_string(text, position, out, error);
}

bool json_bool_field(const std::string& text, const std::string& key, bool* out,
                     std::string* error) {
    size_t position = 0;
    if (!json_find(text, key, &position)) {
        if (error != nullptr) {
            *error = "missing " + key;
        }
        return false;
    }
    if (text.compare(position, 4, "true") == 0) {
        *out = true;
        return true;
    }
    if (text.compare(position, 5, "false") == 0) {
        *out = false;
        return true;
    }
    if (error != nullptr) {
        *error = key + " is not a boolean";
    }
    return false;
}

bool write_install_record(const VirtualDisplayInstallRecord& record, std::string* error) {
    return write_text_file(virtual_display_state_path(),
                           virtual_display_install_json(record), error);
}

bool read_install_record(VirtualDisplayInstallRecord* record) {
    std::string text;
    if (!read_text_file(virtual_display_state_path(), &text)) {
        return false;
    }
    std::string error;
    return parse_virtual_display_install_json(text, record, &error);
}

bool parse_uint32(const std::wstring& text, uint32_t* value) {
    if (text.empty()) {
        return false;
    }
    std::string ascii;
    ascii.reserve(text.size());
    for (const wchar_t c : text) {
        if (c < 0 || c > 0x7f) {
            return false;
        }
        ascii.push_back(static_cast<char>(c));
    }
    unsigned long long parsed = 0;
    const auto result = std::from_chars(ascii.data(), ascii.data() + ascii.size(), parsed);
    if (result.ec != std::errc() || result.ptr != ascii.data() + ascii.size() ||
        parsed > 0xffffffffull) {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

}  // namespace

std::string virtual_display_install_json(const VirtualDisplayInstallRecord& record) {
    std::string json = "{\n";
    json += "  \"provider\": \"" + json_escape(record.provider) + "\",\n";
    json += "  \"package_version\": \"" + json_escape(record.package_version) + "\",\n";
    json += "  \"package_sha256\": \"" + json_escape(record.package_sha256) + "\",\n";
    json += "  \"installed_at\": \"" + json_escape(record.installed_at) + "\",\n";
    json += std::string("  \"owned\": ") + (record.owned ? "true" : "false") + ",\n";
    json += "  \"last_error\": \"" + json_escape(record.last_error) + "\"\n";
    json += "}\n";
    return json;
}

bool parse_virtual_display_install_json(const std::string& text,
                                        VirtualDisplayInstallRecord* record,
                                        std::string* error) {
    if (record == nullptr) {
        if (error != nullptr) {
            *error = "no install record output";
        }
        return false;
    }
    if (text.find('{') == std::string::npos || text.find('}') == std::string::npos) {
        if (error != nullptr) {
            *error = "the install record is not a JSON object";
        }
        return false;
    }
    VirtualDisplayInstallRecord parsed;
    if (!json_string_field(text, "provider", &parsed.provider, error) ||
        !json_string_field(text, "package_version", &parsed.package_version, error) ||
        !json_string_field(text, "package_sha256", &parsed.package_sha256, error) ||
        !json_string_field(text, "installed_at", &parsed.installed_at, error) ||
        !json_bool_field(text, "owned", &parsed.owned, error) ||
        !json_string_field(text, "last_error", &parsed.last_error, error)) {
        return false;
    }
    *record = std::move(parsed);
    return true;
}

std::string virtual_display_timestamp_utc() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                  time.wSecond);
    return buffer;
}

VdisplayCommand parse_vdisplay_command(const std::vector<std::wstring>& args) {
    VdisplayCommand command;
    if (args.empty()) {
        command.error = "missing vdisplay action";
        return command;
    }
    // --log is pulled out first so a usage error still reaches the log the
    // caller asked for instead of the default under %ProgramData%.
    std::vector<std::wstring> rest;
    for (size_t index = 0; index < args.size(); ++index) {
        if (args[index] == L"--log" && index + 1 < args.size()) {
            command.log_path = args[++index];
        } else {
            rest.push_back(args[index]);
        }
    }
    if (rest.empty()) {
        command.error = "missing vdisplay action";
        return command;
    }
    const std::wstring& action = rest.front();
    if (action == L"install") {
        command.action = VdisplayAction::Install;
    } else if (action == L"uninstall") {
        command.action = VdisplayAction::Uninstall;
    } else if (action == L"set-count") {
        command.action = VdisplayAction::SetCount;
    } else if (action == L"status") {
        command.action = VdisplayAction::Status;
    } else {
        command.error = "unknown vdisplay action; use install, uninstall, set-count or status";
        return command;
    }
    bool count_seen = false;
    bool package_seen = false;
    for (size_t index = 1; index < rest.size(); ++index) {
        const std::wstring& argument = rest[index];
        if (argument == L"--package" && index + 1 < rest.size()) {
            command.package_dir = rest[++index];
            package_seen = true;
        } else if (argument == L"--count" && index + 1 < rest.size()) {
            if (!parse_uint32(rest[++index], &command.count)) {
                command.error = "--count must be an integer from 0 to 4";
                return command;
            }
            count_seen = true;
        } else if (command.action == VdisplayAction::SetCount && !count_seen &&
                   !argument.empty() && argument.front() != L'-' &&
                   parse_uint32(argument, &command.count)) {
            count_seen = true;
        } else {
            command.error = "unexpected vdisplay argument";
            return command;
        }
    }
    if (command.action == VdisplayAction::Install) {
        if (!package_seen || command.package_dir.empty()) {
            command.error = "install requires --package <directory>";
            return command;
        }
        if (!count_seen) {
            command.count = 1;
        }
        if (command.count == 0 || command.count > kMaxVirtualDisplayMonitors) {
            command.error = "install --count must be from 1 to 4";
            return command;
        }
    } else if (command.action == VdisplayAction::SetCount) {
        if (!count_seen) {
            command.error = "set-count needs a count from 0 to 4";
            return command;
        }
        if (command.count > kMaxVirtualDisplayMonitors) {
            command.error = "set-count must be from 0 to 4";
            return command;
        }
    } else if (package_seen || count_seen) {
        command.error = "this vdisplay action takes neither --package nor --count";
        return command;
    }
    return command;
}

int run_vdisplay_command(const std::vector<std::wstring>& args) {
    VdisplayCommand command = parse_vdisplay_command(args);
    const std::wstring log_path =
        command.log_path.empty() ? virtual_display_log_path() : command.log_path;
    const auto log = [&log_path](const std::string& line) { append_log(log_path, line); };
    if (!command.error.empty()) {
        log("usage: " + command.error);
        return kVdisplayExitUsage;
    }
    std::string platform_reason;
    if (!virtual_display_platform_supported(&platform_reason)) {
        log("unsupported: " + platform_reason);
        return kVdisplayExitUnsupported;
    }
    if (command.action == VdisplayAction::Status) {
        const VirtualDisplayStatus status =
            classify_virtual_display_probe(probe_virtual_display());
        log(std::string("status state=") + virtual_display_state_name(status.state) +
            " provider=" + virtual_display_provider_name(status.provider) +
            " active=" + std::to_string(status.active_monitors) +
            " max=" + std::to_string(status.max_monitors) + " detail=" + status.detail);
        return kVdisplayExitOk;
    }
    if (!process_elevated()) {
        log("administrator privileges are required for install, uninstall and set-count");
        return kVdisplayExitUsage;
    }
    if (command.action == VdisplayAction::Install) {
        std::string detail;
        std::string version;
        std::string package_sha256;
        const VirtualDisplayPackageStatus check = verify_virtual_display_package(
            command.package_dir, &detail, &version, &package_sha256);
        if (check != VirtualDisplayPackageStatus::Ok) {
            // A rejected package changes nothing, so an existing install
            // record keeps its ownership and only the error is refreshed.
            VirtualDisplayInstallRecord record;
            VirtualDisplayInstallRecord previous;
            if (read_install_record(&previous)) {
                record = previous;
            } else {
                record.owned = false;
            }
            if (!version.empty()) {
                record.package_version = version;
            }
            if (!package_sha256.empty()) {
                record.package_sha256 = package_sha256;
            }
            if (record.installed_at.empty()) {
                record.installed_at = virtual_display_timestamp_utc();
            }
            record.last_error = detail;
            std::string ignored;
            write_install_record(record, &ignored);
            log("package rejected: " + detail);
            if (check == VirtualDisplayPackageStatus::SignatureMismatch) {
                return kVdisplayExitSignature;
            }
            if (check == VirtualDisplayPackageStatus::Unsupported) {
                return kVdisplayExitUnsupported;
            }
            return kVdisplayExitPackage;
        }
        std::unique_ptr<VirtualDisplayProvider> provider = make_virtual_display_provider(
            VirtualDisplayProviderKind::Usbmmidd, command.package_dir);
        // A driver that was already present belongs to whoever installed it:
        // this run may reuse it but must not claim it for later uninstall.
        const VirtualDisplayProbe before_install = probe_virtual_display();
        const bool already_present =
            before_install.adapter_present || before_install.service_registered;
        VirtualDisplayInstallRecord previous;
        const bool have_previous = read_install_record(&previous);
        std::string error;
        if (!provider->install(&error)) {
            VirtualDisplayInstallRecord record;
            if (have_previous) {
                record = previous;
            }
            record.package_version = version;
            record.package_sha256 = package_sha256;
            if (record.installed_at.empty()) {
                record.installed_at = virtual_display_timestamp_utc();
            }
            if (!record.owned) {
                record.owned =
                    provider->status().state != VirtualDisplayState::NotInstalled;
            }
            record.last_error = error;
            std::string ignored;
            write_install_record(record, &ignored);
            log("install failed: " + error);
            return error.find("timed out") != std::string::npos ? kVdisplayExitNoDevice
                                                                : kVdisplayExitInstall;
        }
        if (!provider->set_monitor_count(command.count, &error)) {
            log("monitor count failed after install: " + error);
            return kVdisplayExitNoDevice;
        }
        VirtualDisplayInstallRecord record;
        if (have_previous) {
            record.installed_at = previous.installed_at;
        }
        if (record.installed_at.empty()) {
            record.installed_at = virtual_display_timestamp_utc();
        }
        record.package_version = version;
        record.package_sha256 = package_sha256;
        record.owned = (have_previous && previous.owned) || !already_present;
        std::string ignored;
        write_install_record(record, &ignored);
        const VirtualDisplayStatus status = provider->status();
        log("installed usbmmidd with " + std::to_string(status.active_monitors) +
            " active monitor(s)");
        return kVdisplayExitOk;
    }
    if (command.action == VdisplayAction::Uninstall) {
        VirtualDisplayInstallRecord record;
        if (!read_install_record(&record) || !record.owned) {
            log("the usbmmidd driver was not installed by chast; leaving it in place");
            return kVdisplayExitOk;
        }
        std::unique_ptr<VirtualDisplayProvider> provider = make_virtual_display_provider(
            VirtualDisplayProviderKind::Usbmmidd, default_virtual_display_package_dir());
        std::string error;
        if (!provider->uninstall(&error)) {
            record.last_error = error;
            std::string ignored;
            write_install_record(record, &ignored);
            log("uninstall failed: " + error);
            return kVdisplayExitUninstall;
        }
        DeleteFileW(virtual_display_state_path().c_str());
        log("the usbmmidd driver was removed");
        return kVdisplayExitOk;
    }
    std::unique_ptr<VirtualDisplayProvider> provider = make_virtual_display_provider(
        VirtualDisplayProviderKind::Usbmmidd, default_virtual_display_package_dir());
    if (provider->status().state == VirtualDisplayState::NotInstalled) {
        log("set-count failed: usbmmidd is not installed");
        return kVdisplayExitInstall;
    }
    std::string error;
    if (!provider->set_monitor_count(command.count, &error)) {
        log("set-count failed: " + error);
        return error.find("at most") != std::string::npos ? kVdisplayExitCount
                                                          : kVdisplayExitNoDevice;
    }
    log("monitor count set to " + std::to_string(command.count));
    return kVdisplayExitOk;
}

bool virtual_display_platform_supported(std::string* reason) {
    auto fail = [reason](const char* text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return false;
    };
#if defined(_M_ARM64) || defined(__aarch64__)
    return fail("usbmmidd ships an x64 driver only; ARM64 is not supported");
#elif defined(_M_X64) || defined(__x86_64__)
    // VerifyVersionInfo reports the compatibility manifest's version, so the
    // real build number is read from RtlGetVersion instead.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto get_version =
        ntdll != nullptr
            ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
            : nullptr;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (get_version == nullptr || get_version(&version) != 0) {
        return fail("the Windows version could not be read");
    }
    if (version.dwMajorVersion < 10 ||
        (version.dwMajorVersion == 10 && version.dwBuildNumber < 17763)) {
        return fail("usbmmidd needs Windows 10 1809 or newer");
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
#else
    return fail("only the x64 build supports a virtual display");
#endif
}

}  // namespace chast
