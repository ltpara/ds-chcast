#define NOMINMAX

#include "chast/file_transfer.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

namespace {

constexpr uint8_t kSidecarVersion = 1;
constexpr char kSidecarMagic[9] = {'C', 'H', 'A', 'S', 'P', 'A', 'R', 'T', '1'};
constexpr size_t kSidecarBytes = sizeof(kSidecarMagic) + 1 + 8 + 32;
constexpr size_t kIoChunk = 1024u * 1024u;
// Intermediates live in a reserved directory with hashed names, so they can
// never collide with a user file such as "backup.zip.part".
constexpr wchar_t kPartialDirectory[] = L".chast-partial";

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), wide.data(), length) != length) {
        return {};
    }
    return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 0) {
        return {};
    }
    std::string text(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), text.data(), length, nullptr,
                            nullptr) != length) {
        return {};
    }
    return text;
}

std::wstring long_path(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::wstring absolute_path(const std::wstring& path) {
    if (path.empty() || path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (size == 0) {
        return {};
    }
    std::wstring full(size, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
    if (written == 0 || written >= size) {
        return {};
    }
    full.resize(written);
    return full;
}

std::wstring join(const std::wstring& directory, const std::wstring& name) {
    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += name;
    return path;
}

bool path_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(long_path(path).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    return GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND;
}

// Path-based checks race with a swap; after opening with
// FILE_FLAG_OPEN_REPARSE_POINT the attributes of the object actually held
// decide whether the handle is a plain file or directory.
bool plain_handle(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(handle, &info) != FALSE &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
           ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

bool ensure_partial_directory(const std::wstring& parent, std::wstring& directory,
                              std::string* error) {
    directory = join(parent, kPartialDirectory);
    if (!CreateDirectoryW(long_path(directory).c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        if (error) {
            *error = "cannot create the partial directory (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    HANDLE handle = CreateFileW(long_path(directory).c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "the partial path is not a directory";
        }
        return false;
    }
    const bool plain = plain_handle(handle, true);
    CloseHandle(handle);
    if (!plain) {
        if (error) {
            *error = "the partial path is a reparse point";
        }
        return false;
    }
    return true;
}

std::wstring partial_file_name(const std::wstring& final_name) {
    const std::string text = wide_to_utf8(final_name);
    Sha256 hash;
    std::array<uint8_t, 32> digest{};
    if (text.empty() || !hash.ready() ||
        !hash.update(text.data(), text.size()) || !hash.finish(digest)) {
        return {};
    }
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring name;
    name.reserve(32 + 5);
    for (size_t i = 0; i < 16; ++i) {
        name.push_back(kHex[digest[i] >> 4]);
        name.push_back(kHex[digest[i] & 0x0F]);
    }
    name += L".part";
    return name;
}

bool ensure_directory(const std::wstring& path, std::string* error) {
    const std::wstring long_directory = long_path(path);
    const int result = SHCreateDirectoryExW(nullptr, long_directory.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
        result == ERROR_FILE_EXISTS) {
        return true;
    }
    if (error) {
        *error = "cannot create the receive directory (error " + std::to_string(result) + ")";
    }
    return false;
}

bool free_space(const std::wstring& directory, uint64_t& bytes) {
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr)) {
        return false;
    }
    bytes = available.QuadPart;
    return true;
}

bool valid_reserved_stem(const std::wstring& stem) {
    std::wstring upper = stem;
    for (wchar_t& c : upper) {
        c = static_cast<wchar_t>(std::towupper(c));
    }
    if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL") {
        return true;
    }
    if (upper.size() == 4 &&
        (upper.compare(0, 3, L"COM") == 0 || upper.compare(0, 3, L"LPT") == 0) &&
        upper[3] >= L'1' && upper[3] <= L'9') {
        return true;
    }
    return false;
}

std::wstring sanitize_wide(std::wstring name) {
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        name = name.substr(slash + 1);
    }
    std::wstring clean;
    clean.reserve(name.size());
    for (wchar_t c : name) {
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
            c == L'|' || c == L'?' || c == L'*') {
            clean.push_back(L'_');
            continue;
        }
        clean.push_back(c);
    }
    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) {
        clean.pop_back();
    }
    if (clean.empty()) {
        clean = L"file";
    }
    if (clean.size() > kMaxReceiveNameChars) {
        // Keep the extension when the stem has to be cut; a received file
        // without it no longer opens with the right program.
        std::wstring extension;
        const size_t dot = clean.find_last_of(L'.');
        if (dot != std::wstring::npos && dot != 0 &&
            clean.size() - dot <= kMaxReceiveNameChars / 2) {
            extension = clean.substr(dot);
        }
        size_t cut = kMaxReceiveNameChars - extension.size();
        // Never split a surrogate pair; the name would not round-trip through
        // the API as UTF-16.
        if (cut > 0 && clean[cut - 1] >= 0xD800 && clean[cut - 1] <= 0xDBFF) {
            --cut;
        }
        clean.resize(cut);
        while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) {
            clean.pop_back();
        }
        clean += extension;
    }
    if (clean.empty()) {
        clean = L"file";
    }
    std::wstring stem = clean;
    const size_t dot = clean.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) {
        stem = clean.substr(0, dot);
    }
    if (valid_reserved_stem(stem)) {
        clean.insert(clean.begin(), L'_');
    }
    return clean;
}

// Walks the relative directory one component at a time. Existing components
// must be plain directories; nothing is created through a reparse point.
bool ensure_subdirectory_chain(const std::wstring& root, const std::wstring& relative,
                               std::wstring& final_directory, std::string* error) {
    final_directory = root;
    size_t start = 0;
    while (start < relative.size()) {
        const size_t slash = relative.find(L'/', start);
        const std::wstring part =
            relative.substr(start, slash == std::wstring::npos ? std::wstring::npos
                                                               : slash - start);
        const std::wstring next = join(final_directory, part);
        DWORD attributes = GetFileAttributesW(long_path(next).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(long_path(next).c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                if (error) {
                    *error = "cannot create the receive directory (error " +
                             std::to_string(GetLastError()) + ")";
                }
                return false;
            }
            attributes = GetFileAttributesW(long_path(next).c_str());
        }
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (error) {
                *error = "the receive path crosses a file named " + wide_to_utf8(part);
            }
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (error) {
                *error = "the receive path is a reparse point";
            }
            return false;
        }
        final_directory = next;
        if (slash == std::wstring::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

std::wstring unique_name(const std::wstring& directory, const std::wstring& name,
                         std::string* error) {
    if (!path_exists(join(directory, name))) {
        return name;
    }
    std::wstring stem = name;
    std::wstring extension;
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) {
        stem = name.substr(0, dot);
        extension = name.substr(dot);
    }
    for (int index = 1; index < 10000; ++index) {
        const std::wstring suffix = L" (" + std::to_wstring(index) + L")";
        std::wstring candidate = stem;
        if (candidate.size() + suffix.size() + extension.size() > kMaxReceiveNameChars) {
            const size_t keep = kMaxReceiveNameChars > suffix.size() + extension.size()
                                    ? kMaxReceiveNameChars - suffix.size() - extension.size()
                                    : 0;
            candidate.resize(keep);
            while (!candidate.empty() &&
                   (candidate.back() == L' ' || candidate.back() == L'.')) {
                candidate.pop_back();
            }
        }
        candidate += suffix;
        candidate += extension;
        if (!path_exists(join(directory, candidate))) {
            return candidate;
        }
    }
    if (error) {
        *error = "no free name is left in the receive directory";
    }
    return {};
}

struct Sidecar {
    uint64_t size = 0;
    std::array<uint8_t, 32> digest{};
};

bool read_sidecar(const std::wstring& path, Sidecar& sidecar) {
    HANDLE file = CreateFileW(long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!plain_handle(file, false)) {
        CloseHandle(file);
        return false;
    }
    std::array<uint8_t, kSidecarBytes> bytes{};
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                             nullptr) != FALSE &&
                    read == bytes.size();
    CloseHandle(file);
    if (!ok || std::memcmp(bytes.data(), kSidecarMagic, sizeof(kSidecarMagic)) != 0 ||
        bytes[sizeof(kSidecarMagic)] != kSidecarVersion) {
        return false;
    }
    size_t cursor = sizeof(kSidecarMagic) + 1;
    sidecar.size = 0;
    for (int i = 0; i < 8; ++i) {
        sidecar.size |= static_cast<uint64_t>(bytes[cursor + i]) << (8 * i);
    }
    cursor += 8;
    std::memcpy(sidecar.digest.data(), bytes.data() + cursor, sidecar.digest.size());
    return true;
}

bool write_sidecar(const std::wstring& path, uint64_t size,
                   const std::array<uint8_t, 32>& digest) {
    HANDLE file = CreateFileW(long_path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!plain_handle(file, false)) {
        CloseHandle(file);
        return false;
    }
    std::array<uint8_t, kSidecarBytes> bytes{};
    std::memcpy(bytes.data(), kSidecarMagic, sizeof(kSidecarMagic));
    size_t cursor = sizeof(kSidecarMagic);
    bytes[cursor++] = kSidecarVersion;
    for (int i = 0; i < 8; ++i) {
        bytes[cursor++] = static_cast<uint8_t>(size >> (8 * i));
    }
    std::memcpy(bytes.data() + cursor, digest.data(), digest.size());
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr) != FALSE &&
                    written == bytes.size();
    CloseHandle(file);
    return ok;
}

}  // namespace

std::string sanitize_received_name(const std::string& utf8_name) {
    const std::wstring wide = utf8_to_wide(utf8_name);
    if (wide.empty()) {
        return "file";
    }
    const std::string name = wide_to_utf8(sanitize_wide(wide));
    return name.empty() ? "file" : name;
}

bool sanitize_received_directory(const std::string& utf8_path, std::string& sanitized) {
    sanitized.clear();
    if (utf8_path.empty()) {
        return true;
    }
    if (utf8_path.size() > kMaxFileRelativePathBytes) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(utf8_path);
    if (wide.empty() || wide.front() == L'/' || wide.back() == L'/' ||
        wide.find(L'\\') != std::wstring::npos ||
        wide.find(L':') != std::wstring::npos) {
        return false;
    }
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= wide.size()) {
        const size_t end = wide.find(L'/', start);
        const size_t stop = end == std::wstring::npos ? wide.size() : end;
        std::wstring part = wide.substr(start, stop - start);
        if (part.empty() || part == L"." || part == L"..") {
            return false;
        }
        part = sanitize_wide(std::move(part));
        if (part.empty() || part == L"." || part == L"..") {
            return false;
        }
        // The reserved name holds half-received files; a transfer must not
        // place its own directories or files there.
        if (_wcsicmp(part.c_str(), kPartialDirectory) == 0) {
            return false;
        }
        parts.push_back(std::move(part));
        if (parts.size() > kMaxReceivePathDepth) {
            return false;
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    std::wstring joined;
    for (const std::wstring& part : parts) {
        if (!joined.empty()) {
            joined.push_back(L'/');
        }
        joined += part;
    }
    sanitized = wide_to_utf8(joined);
    return !sanitized.empty() && sanitized.size() <= kMaxFileRelativePathBytes;
}

bool create_receive_directory(const std::string& root, const std::string& relative,
                              std::string* error) {
    std::string safe;
    if (relative.empty() || !sanitize_received_directory(relative, safe) || safe != relative) {
        if (error) *error = "the clipboard directory path is not usable";
        return false;
    }
    const auto directory = absolute_path(utf8_to_wide(root));
    const DWORD attributes = directory.empty() ? INVALID_FILE_ATTRIBUTES :
        GetFileAttributesW(long_path(directory).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (error) *error = "the clipboard staging root is not a plain directory";
        return false;
    }
    std::wstring created;
    return ensure_subdirectory_chain(directory, utf8_to_wide(safe), created, error);
}

std::string default_receive_directory() {
    PWSTR downloads = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr,
                                       &downloads)) &&
        downloads != nullptr) {
        std::wstring directory(downloads);
        CoTaskMemFree(downloads);
        directory += L"\\chast";
        return wide_to_utf8(directory);
    }
    return {};
}

bool sha256_file(const std::string& utf8_path, uint64_t& size,
                 std::array<uint8_t, 32>& digest, std::string* error) {
    size = 0;
    digest.fill(0);
    const std::wstring path = absolute_path(utf8_to_wide(utf8_path));
    if (path.empty()) {
        if (error) {
            *error = "the file path is not usable";
        }
        return false;
    }
    HANDLE file = CreateFileW(long_path(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "cannot open " + utf8_path + " (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    LARGE_INTEGER total{};
    if (!GetFileSizeEx(file, &total) || total.QuadPart < 0) {
        CloseHandle(file);
        if (error) {
            *error = "cannot read the size of " + utf8_path;
        }
        return false;
    }
    if (static_cast<uint64_t>(total.QuadPart) > kMaxFileTransferBytes) {
        CloseHandle(file);
        if (error) {
            *error = "the file is past the protocol limit";
        }
        return false;
    }
    Sha256 hash;
    if (!hash.ready()) {
        CloseHandle(file);
        if (error) {
            *error = "SHA-256 is unavailable";
        }
        return false;
    }
    std::vector<uint8_t> buffer(kIoChunk);
    uint64_t read_total = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            CloseHandle(file);
            if (error) {
                *error = "cannot read " + utf8_path + " (error " +
                         std::to_string(GetLastError()) + ")";
            }
            return false;
        }
        if (read == 0) {
            break;
        }
        if (!hash.update(buffer.data(), read)) {
            CloseHandle(file);
            if (error) {
                *error = "SHA-256 failed";
            }
            return false;
        }
        read_total += read;
        if (read_total > kMaxFileTransferBytes) {
            CloseHandle(file);
            if (error) {
                *error = "the file grew past the protocol limit while it was read";
            }
            return false;
        }
    }
    CloseHandle(file);
    if (!hash.finish(digest)) {
        if (error) {
            *error = "SHA-256 failed";
        }
        return false;
    }
    size = read_total;
    return true;
}

struct FileReceiver::Impl {
    std::wstring directory;
    std::wstring final_name;
    std::wstring final_path;
    std::wstring part_path;
    std::wstring sidecar_path;
    std::string name;
    uint64_t size = 0;
    uint64_t received = 0;
    std::array<uint8_t, 32> digest{};
    HANDLE file = INVALID_HANDLE_VALUE;
    Sha256 hash;
};

namespace {

// 100 ns ticks between the FILETIME epoch (1601-01-01) and Unix time.
constexpr uint64_t kFileTimeUnixEpoch = 116444736000000000ull;

}  // namespace

FileCode resolve_receive_path(const std::string& root, const std::string& relative,
                              std::string& full_path, bool& is_directory,
                              std::string& error) {
    full_path.clear();
    is_directory = false;
    // The protocol layer already validated the wire path; this second pass is
    // the sandbox boundary, so it does not trust it.
    if (relative.size() > kMaxFileBrowserPathBytes ||
        relative.find('\\') != std::string::npos ||
        relative.find(':') != std::string::npos ||
        relative.find('\0') != std::string::npos) {
        error = "the path is not a plain relative path";
        return FileCode::Refused;
    }
    const std::wstring wide_root = utf8_to_wide(root);
    if (wide_root.empty()) {
        error = "the receive directory is not usable";
        return FileCode::Refused;
    }
    std::wstring current = wide_root;
    if (relative.empty()) {
        const DWORD attributes = GetFileAttributesW(long_path(wide_root).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                error = "the receive directory does not exist";
                return FileCode::NotFound;
            }
            error = "cannot read the receive directory (error " + std::to_string(code) + ")";
            return FileCode::IoError;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            error = "the receive directory is not a plain directory";
            return FileCode::Refused;
        }
        is_directory = true;
    } else {
        size_t start = 0;
        size_t components = 0;
        for (;;) {
            const size_t slash = relative.find('/', start);
            const size_t length =
                slash == std::string::npos ? relative.size() - start : slash - start;
            if (++components > kMaxFileRelativeComponents) {
                error = "the path is too deep";
                return FileCode::Refused;
            }
            if (length == 0 || length > kMaxFileNameBytes ||
                (length == 1 && relative[start] == '.') ||
                (length == 2 && relative[start] == '.' && relative[start + 1] == '.')) {
                error = "a path component is not usable";
                return FileCode::Refused;
            }
            const std::wstring wide_part = utf8_to_wide(relative.substr(start, length));
            if (wide_part.empty()) {
                error = "a path component is not usable";
                return FileCode::Refused;
            }
            if (_wcsicmp(wide_part.c_str(), kPartialDirectory) == 0) {
                error = "the path is reserved for partial transfers";
                return FileCode::Refused;
            }
            current = join(current, wide_part);
            const DWORD attributes = GetFileAttributesW(long_path(current).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD code = GetLastError();
                if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                    error = "no such file or directory";
                    return FileCode::NotFound;
                }
                error = "cannot read the path (error " + std::to_string(code) + ")";
                return FileCode::IoError;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                error = "the path crosses a reparse point";
                return FileCode::Refused;
            }
            const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!directory && slash != std::string::npos) {
                error = "a path component is not a directory";
                return FileCode::NotFound;
            }
            is_directory = directory;
            if (slash == std::string::npos) {
                break;
            }
            start = slash + 1;
        }
    }
    const std::wstring absolute = absolute_path(current);
    full_path = wide_to_utf8(absolute.empty() ? current : absolute);
    if (full_path.empty()) {
        error = "the path cannot be represented";
        return FileCode::Refused;
    }
    return FileCode::Ok;
}

namespace { bool valid_operation_name(const std::string& name); }

void list_receive_directory(const std::string& root, const std::string& relative,
                            FileListResult& result, std::string cursor) {
    result.entries.clear();
    result.truncated = false;
    result.next_cursor.clear();
    result.code = FileCode::Ok;
    result.reason.clear();
    if (!cursor.empty() && (cursor.size() <= 2 || (cursor[0] != 'd' && cursor[0] != 'f') ||
        cursor[1] != ':' || !valid_operation_name(cursor.substr(2)))) {
        result.code = FileCode::Refused;
        result.reason = "invalid directory continuation cursor";
        return;
    }
    FileListEntry after;
    if (!cursor.empty()) { after.directory = cursor[0] == 'd'; after.name = cursor.substr(2); }

    std::string directory;
    bool is_directory = false;
    std::string error;
    result.code = resolve_receive_path(root, relative, directory, is_directory, error);
    if (result.code != FileCode::Ok) {
        result.reason = error;
        return;
    }
    if (!is_directory) {
        result.code = FileCode::NotFound;
        result.reason = "the path is not a directory";
        return;
    }

    // FindFirstFile rather than std::filesystem so a reparse point can be
    // spotted from the enumeration data without resolving it.
    const std::wstring pattern = join(utf8_to_wide(directory), L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(long_path(pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            return;  // an empty directory
        }
        result.code = FileCode::IoError;
        result.reason = "cannot list the directory (error " + std::to_string(code) + ")";
        return;
    }

    auto& page = result.entries;
    page.reserve(kMaxFileListEntries + 1);
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L".." ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        const std::string utf8_name = wide_to_utf8(name);
        // An entry the wire cannot carry would fail the whole answer, so it is
        // skipped like a reparse point.
        if (!valid_operation_name(utf8_name)) {
            continue;
        }
        FileListEntry entry;
        entry.name = utf8_name;
        entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!cursor.empty() && !file_list_entry_less(after, entry)) continue;
        if (!entry.directory) {
            ULARGE_INTEGER size{};
            size.LowPart = data.nFileSizeLow;
            size.HighPart = data.nFileSizeHigh;
            entry.size = size.QuadPart;
        }
        ULARGE_INTEGER stamp{};
        stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
        stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
        if (stamp.QuadPart >= kFileTimeUnixEpoch) {
            entry.modified_ms = (stamp.QuadPart - kFileTimeUnixEpoch) / 10000;
        }
        // Keep the smallest page plus one lookahead, regardless of the
        // filesystem's enumeration order or the directory's total size.
        if (page.size() < kMaxFileListEntries + 1) {
            page.push_back(std::move(entry));
            std::push_heap(page.begin(), page.end(), file_list_entry_less);
        } else if (file_list_entry_less(entry, page.front())) {
            std::pop_heap(page.begin(), page.end(), file_list_entry_less);
            page.back() = std::move(entry);
            std::push_heap(page.begin(), page.end(), file_list_entry_less);
        }
    } while (FindNextFileW(find, &data));
    const DWORD enumeration_error = GetLastError();
    FindClose(find);
    if (enumeration_error != ERROR_NO_MORE_FILES) {
        page.clear();
        result.code = FileCode::IoError;
        result.reason = "cannot finish listing the directory (error " + std::to_string(enumeration_error) + ")";
        return;
    }
    std::sort_heap(page.begin(), page.end(), file_list_entry_less);
    result.truncated = page.size() > kMaxFileListEntries;
    if (result.truncated) {
        page.pop_back();
        result.next_cursor = file_list_cursor(page.back());
    }
}

namespace {

bool valid_operation_name(const std::string& name) {
    return !name.empty() && name.size() <= kMaxFileNameBytes &&
           name.find('\0') == std::string::npos &&
           name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos &&
           name.find(':') == std::string::npos && name != "." && name != ".." &&
           !utf8_to_wide(name).empty();
}

FileCode map_operation_error(DWORD code, std::string& error) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            error = "no such file or directory";
            return FileCode::NotFound;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            error = "an entry with that name already exists";
            return FileCode::Refused;
        case ERROR_DIR_NOT_EMPTY:
            error = "the folder is not empty";
            return FileCode::Refused;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            error = "the host refused the change (error " + std::to_string(code) + ")";
            return FileCode::Refused;
        default:
            error = "the host's file system failed (error " + std::to_string(code) + ")";
            return FileCode::IoError;
    }
}

}  // namespace

FileCode apply_file_operation(const std::string& root, FileOperation op,
                              const std::string& path, const std::string& new_name,
                              std::string& error) {
    error.clear();
    if (path.empty() || path.size() > kMaxFileBrowserPathBytes) {
        error = "the path is not a plain relative path";
        return FileCode::Refused;
    }
    const size_t slash = path.rfind('/');
    const std::string parent =
        slash == std::string::npos ? std::string() : path.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    if (!valid_operation_name(leaf)) {
        error = "the entry name is not usable";
        return FileCode::Refused;
    }

    std::string parent_path;
    bool parent_is_directory = false;
    FileCode code =
        resolve_receive_path(root, parent, parent_path, parent_is_directory, error);
    if (code != FileCode::Ok) {
        return code;
    }
    if (!parent_is_directory) {
        error = "the parent path is not a directory";
        return FileCode::Refused;
    }
    const std::wstring wide_parent = utf8_to_wide(parent_path);
    const std::wstring wide_leaf = utf8_to_wide(leaf);
    if (wide_parent.empty() || wide_leaf.empty()) {
        error = "the entry name is not usable";
        return FileCode::Refused;
    }
    const std::wstring target = join(wide_parent, wide_leaf);

    switch (op) {
        case FileOperation::MakeDirectory:
            if (!CreateDirectoryW(long_path(target).c_str(), nullptr)) {
                return map_operation_error(GetLastError(), error);
            }
            return FileCode::Ok;
        case FileOperation::Rename: {
            if (!valid_operation_name(new_name)) {
                error = "the new name is not usable";
                return FileCode::Refused;
            }
            const std::wstring wide_new = utf8_to_wide(new_name);
            if (wide_new.empty()) {
                error = "the new name is not usable";
                return FileCode::Refused;
            }
            const DWORD attributes = GetFileAttributesW(long_path(target).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                return map_operation_error(GetLastError(), error);
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                error = "the path is a reparse point";
                return FileCode::Refused;
            }
            const std::wstring destination = join(wide_parent, wide_new);
            if (!MoveFileExW(long_path(target).c_str(), long_path(destination).c_str(),
                             MOVEFILE_WRITE_THROUGH)) {
                return map_operation_error(GetLastError(), error);
            }
            return FileCode::Ok;
        }
        case FileOperation::Remove: {
            const DWORD attributes = GetFileAttributesW(long_path(target).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                return map_operation_error(GetLastError(), error);
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                error = "the path is a reparse point";
                return FileCode::Refused;
            }
            if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
                error = "the entry is read-only";
                return FileCode::Refused;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!RemoveDirectoryW(long_path(target).c_str())) {
                    return map_operation_error(GetLastError(), error);
                }
            } else if (!DeleteFileW(long_path(target).c_str())) {
                return map_operation_error(GetLastError(), error);
            }
            return FileCode::Ok;
        }
    }
    error = "the operation is not supported";
    return FileCode::Refused;
}

FileReceiver::FileReceiver() : impl_(std::make_unique<Impl>()) {}
FileReceiver::~FileReceiver() {
    if (impl_ && impl_->file != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->file);
    }
}

std::unique_ptr<FileReceiver> FileReceiver::create(const FileReceiveOptions& options,
                                                   uint64_t* resume_offset,
                                                   std::string* error) {
    if (resume_offset) {
        *resume_offset = 0;
    }
    std::unique_ptr<FileReceiver> receiver(new FileReceiver());
    Impl& impl = *receiver->impl_;
    impl.size = options.size;
    impl.digest = options.sha256;
    if (!impl.hash.ready()) {
        if (error) {
            *error = "SHA-256 is unavailable";
        }
        return nullptr;
    }

    const std::wstring directory = absolute_path(utf8_to_wide(options.directory));
    if (directory.empty()) {
        if (error) {
            *error = "the receive directory is not usable";
        }
        return nullptr;
    }
    if (!ensure_directory(directory, error)) {
        return nullptr;
    }
    std::wstring partial_directory;
    if (!ensure_partial_directory(directory, partial_directory, error)) {
        return nullptr;
    }
    std::string relative;
    if (!sanitize_received_directory(options.relative_path, relative)) {
        if (error) {
            *error = "the offered folder path is not usable";
        }
        return nullptr;
    }
    std::wstring final_directory;
    if (!ensure_subdirectory_chain(directory, utf8_to_wide(relative), final_directory,
                                   error)) {
        return nullptr;
    }
    impl.directory = final_directory;

    const std::wstring sanitized = sanitize_wide(utf8_to_wide(options.name));
    impl.name = wide_to_utf8(sanitized);
    impl.final_name = unique_name(final_directory, sanitized, error);
    if (impl.final_name.empty()) {
        return nullptr;
    }
    impl.final_path = join(final_directory, impl.final_name);
    const std::wstring part_name = partial_file_name(impl.final_name);
    if (part_name.empty()) {
        if (error) {
            *error = "cannot name the partial file";
        }
        return nullptr;
    }
    impl.part_path = join(partial_directory, part_name);
    impl.sidecar_path = impl.part_path + L".chast";

    uint64_t resume = 0;
    bool resume_ok = false;
    if (options.allow_resume && path_exists(impl.part_path)) {
        Sidecar sidecar;
        if (read_sidecar(impl.sidecar_path, sidecar) && sidecar.size == options.size &&
            sidecar.digest == options.sha256) {
            HANDLE existing = CreateFileW(long_path(impl.part_path).c_str(),
                                          GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                              FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr);
            if (existing != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER size{};
                if (plain_handle(existing, false) && GetFileSizeEx(existing, &size) &&
                    size.QuadPart >= 0 &&
                    static_cast<uint64_t>(size.QuadPart) <= options.size) {
                    impl.file = existing;
                    resume = static_cast<uint64_t>(size.QuadPart);
                    resume_ok = true;
                } else {
                    CloseHandle(existing);
                }
            }
        }
    }

    if (resume_ok) {
        impl.received = resume;
        if (resume > 0) {
            LARGE_INTEGER start{};
            if (!SetFilePointerEx(impl.file, start, nullptr, FILE_BEGIN)) {
                CloseHandle(impl.file);
                impl.file = INVALID_HANDLE_VALUE;
                if (error) {
                    *error = "cannot seek in the partial file";
                }
                return nullptr;
            }
            std::vector<uint8_t> buffer(kIoChunk);
            uint64_t left = resume;
            while (left > 0) {
                const DWORD want = static_cast<DWORD>(std::min<uint64_t>(left, kIoChunk));
                DWORD read = 0;
                if (!ReadFile(impl.file, buffer.data(), want, &read, nullptr) || read == 0 ||
                    !impl.hash.update(buffer.data(), read)) {
                    CloseHandle(impl.file);
                    impl.file = INVALID_HANDLE_VALUE;
                    if (error) {
                        *error = "cannot hash the partial file";
                    }
                    return nullptr;
                }
                left -= read;
            }
        }
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(resume);
        SetFilePointerEx(impl.file, end, nullptr, FILE_BEGIN);
    } else {
        impl.file = CreateFileW(long_path(impl.part_path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                    FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
        if (impl.file == INVALID_HANDLE_VALUE) {
            if (error) {
                *error = "cannot create the partial file (error " +
                         std::to_string(GetLastError()) + ")";
            }
            return nullptr;
        }
        if (!plain_handle(impl.file, false)) {
            CloseHandle(impl.file);
            impl.file = INVALID_HANDLE_VALUE;
            if (error) {
                *error = "the partial file is a reparse point";
            }
            return nullptr;
        }
        impl.received = 0;
        if (!write_sidecar(impl.sidecar_path, options.size, options.sha256)) {
            CloseHandle(impl.file);
            impl.file = INVALID_HANDLE_VALUE;
            DeleteFileW(long_path(impl.part_path).c_str());
            if (error) {
                *error = "cannot write the resume sidecar";
            }
            return nullptr;
        }
    }

    uint64_t available = 0;
    if (!free_space(directory, available) ||
        available < (options.size - impl.received) + kMinReceiveFreeSpace) {
        CloseHandle(impl.file);
        impl.file = INVALID_HANDLE_VALUE;
        if (!resume_ok) {
            DeleteFileW(long_path(impl.part_path).c_str());
            DeleteFileW(long_path(impl.sidecar_path).c_str());
        }
        if (error) {
            *error = "not enough free space in the receive directory";
        }
        return nullptr;
    }

    if (resume_offset) {
        *resume_offset = impl.received;
    }
    return receiver;
}

bool FileReceiver::write(const uint8_t* data, size_t size, std::string* error) {
    Impl& impl = *impl_;
    if (impl.file == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "the receiver is not open";
        }
        return false;
    }
    if (size > impl.size - impl.received) {
        if (error) {
            *error = "the stream carries more bytes than the offer";
        }
        return false;
    }
    const uint8_t* cursor = data;
    size_t left = size;
    while (left > 0) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(left, kIoChunk));
        DWORD written = 0;
        if (!WriteFile(impl.file, cursor, want, &written, nullptr) || written == 0) {
            if (error) {
                *error = "cannot write the partial file (error " +
                         std::to_string(GetLastError()) + ")";
            }
            return false;
        }
        if (!impl.hash.update(cursor, written)) {
            if (error) {
                *error = "SHA-256 failed";
            }
            return false;
        }
        impl.received += written;
        cursor += written;
        left -= written;
    }
    return true;
}

bool FileReceiver::finish(std::string& final_path, std::string* error) {
    Impl& impl = *impl_;
    final_path.clear();
    if (impl.file == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "the receiver is not open";
        }
        return false;
    }
    if (impl.received != impl.size) {
        CloseHandle(impl.file);
        impl.file = INVALID_HANDLE_VALUE;
        if (error) {
            *error = "the stream ended at " + std::to_string(impl.received) + " of " +
                     std::to_string(impl.size) + " bytes";
        }
        return false;
    }
    std::array<uint8_t, 32> digest{};
    if (!impl.hash.finish(digest)) {
        CloseHandle(impl.file);
        impl.file = INVALID_HANDLE_VALUE;
        DeleteFileW(long_path(impl.part_path).c_str());
        DeleteFileW(long_path(impl.sidecar_path).c_str());
        if (error) {
            *error = "SHA-256 failed";
        }
        return false;
    }
    if (digest != impl.digest) {
        CloseHandle(impl.file);
        impl.file = INVALID_HANDLE_VALUE;
        DeleteFileW(long_path(impl.part_path).c_str());
        DeleteFileW(long_path(impl.sidecar_path).c_str());
        if (error) {
            *error = "the received bytes do not match the offered SHA-256";
        }
        return false;
    }
    if (!FlushFileBuffers(impl.file)) {
        CloseHandle(impl.file);
        impl.file = INVALID_HANDLE_VALUE;
        if (error) {
            *error = "cannot flush the received file";
        }
        return false;
    }
    CloseHandle(impl.file);
    impl.file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(long_path(impl.part_path).c_str(), long_path(impl.final_path).c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        if (error) {
            *error = "cannot move the received file into place (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    DeleteFileW(long_path(impl.sidecar_path).c_str());
    final_path = wide_to_utf8(impl.final_path);
    return true;
}

void FileReceiver::abort(bool keep_partial) {
    Impl& impl = *impl_;
    if (impl.file != INVALID_HANDLE_VALUE) {
        CloseHandle(impl.file);
        impl.file = INVALID_HANDLE_VALUE;
    }
    if (!keep_partial) {
        DeleteFileW(long_path(impl.part_path).c_str());
        DeleteFileW(long_path(impl.sidecar_path).c_str());
    }
}

uint64_t FileReceiver::received() const {
    return impl_->received;
}

uint64_t FileReceiver::size() const {
    return impl_->size;
}

const std::string& FileReceiver::name() const {
    return impl_->name;
}

}  // namespace chast
