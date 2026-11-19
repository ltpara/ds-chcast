#include "file_snapshot.h"

namespace chast::host::detail {
namespace {

constexpr DWORD kMaximumSnapshotBytes = 4 * 1024 * 1024;

DWORD read_file(const std::wstring& path, std::vector<char>& contents) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return error;
    }
    if (size.QuadPart < 0 || size.QuadPart > kMaximumSnapshotBytes) {
        CloseHandle(file);
        return ERROR_FILE_TOO_LARGE;
    }
    contents.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD request = static_cast<DWORD>(contents.size() - offset);
        DWORD read = 0;
        if (!ReadFile(file, contents.data() + offset, request, &read, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return error;
        }
        if (read == 0) {
            CloseHandle(file);
            return ERROR_HANDLE_EOF;
        }
        offset += read;
    }
    CloseHandle(file);
    return ERROR_SUCCESS;
}

DWORD write_file(const std::wstring& path, const std::vector<char>& contents) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();

    size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD request = static_cast<DWORD>(contents.size() - offset);
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, request, &written, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return error;
        }
        if (written == 0) {
            CloseHandle(file);
            return ERROR_WRITE_FAULT;
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return error;
    }
    CloseHandle(file);
    return ERROR_SUCCESS;
}

}  // namespace

DWORD FileSnapshot::capture(const std::wstring& path) {
    path_ = path;
    contents_.clear();
    captured_ = false;
    existed_ = false;
    if (path_.empty()) return ERROR_INVALID_NAME;

    const DWORD attributes = GetFileAttributesW(path_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return error;
        captured_ = true;
        return ERROR_SUCCESS;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return ERROR_DIRECTORY;

    const DWORD error = read_file(path_, contents_);
    if (error != ERROR_SUCCESS) {
        contents_.clear();
        return error;
    }
    existed_ = true;
    captured_ = true;
    return ERROR_SUCCESS;
}

DWORD FileSnapshot::restore() const {
    if (!captured_) return ERROR_INVALID_STATE;
    if (!existed_) {
        if (DeleteFileW(path_.c_str())) return ERROR_SUCCESS;
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? ERROR_SUCCESS
                   : error;
    }

    const std::wstring temporary = path_ + L".rollback.tmp";
    DWORD error = write_file(temporary, contents_);
    if (error != ERROR_SUCCESS) {
        DeleteFileW(temporary.c_str());
        return error;
    }
    if (ReplaceFileW(path_.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH,
                     nullptr, nullptr)) {
        return ERROR_SUCCESS;
    }
    error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        if (MoveFileExW(temporary.c_str(), path_.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return ERROR_SUCCESS;
        }
        error = GetLastError();
    }
    DeleteFileW(temporary.c_str());
    return error;
}

}  // namespace chast::host::detail
