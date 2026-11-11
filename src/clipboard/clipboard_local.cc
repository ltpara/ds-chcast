#define NOMINMAX
#include "chast/clipboard_virtual.h"
#include "chast/protocol.h"
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>

namespace chast {
namespace {
class LocalFiles final : public ClipboardVirtualFiles {
public:
    explicit LocalFiles(std::vector<ClipboardFileEntry> entries) : entries_(std::move(entries)) {}
    ~LocalFiles() override { for (const auto& file : files_) if (file.handle != INVALID_HANDLE_VALUE) CloseHandle(file.handle); }
    const std::vector<ClipboardFileEntry>& entries() const override { return entries_; }
    bool Active() const override { return active_.load(); }
    uint64_t SourceVersion() const override { return version_; }
    void Cancel() override {
        active_ = false;
        std::lock_guard lock(io_mutex_);
        if (reading_ != INVALID_HANDLE_VALUE) CancelIoEx(reading_, nullptr);
    }
    bool Open(const std::vector<std::string>& paths) {
        auto mix = [&](uint64_t value) {
            for (unsigned i = 0; i < 8; ++i) { version_ ^= (value >> (i * 8)) & 255; version_ *= 1099511628211ull; }
        };
        if (entries_.size() != paths.size() || encode_clipboard_file_list(entries_).empty()) return false;
        files_.reserve(paths.size());
        for (size_t i = 0; i < paths.size(); ++i) {
            File file;
            if (!entries_[i].directory) {
                if (entries_[i].size > kMaxFileTransferBytes) return false;
                const auto path = std::filesystem::absolute(std::filesystem::path(std::u8string(paths[i].begin(), paths[i].end())));
                auto wide = path.native();
                if (wide.rfind(L"\\\\?\\", 0) != 0) wide = wide.rfind(L"\\\\", 0) == 0
                    ? L"\\\\?\\UNC\\" + wide.substr(2) : L"\\\\?\\" + wide;
                file.handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (file.handle == INVALID_HANDLE_VALUE) return false;
                const bool valid = GetFileInformationByHandle(file.handle, &file.info) &&
                    !(file.info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    ((static_cast<uint64_t>(file.info.nFileSizeHigh) << 32) | file.info.nFileSizeLow) == entries_[i].size &&
                    GetFileType(file.handle) == FILE_TYPE_DISK;
                if (!valid) { CloseHandle(file.handle); return false; }
                mix(file.info.dwVolumeSerialNumber);
                mix((static_cast<uint64_t>(file.info.nFileIndexHigh) << 32) | file.info.nFileIndexLow);
                mix((static_cast<uint64_t>(file.info.ftLastWriteTime.dwHighDateTime) << 32) | file.info.ftLastWriteTime.dwLowDateTime);
            }
            for (const unsigned char c : paths[i]) mix(c);
            files_.push_back(file);
        }
        return true;
    }
    bool Read(size_t index, uint64_t offset, uint32_t length, std::vector<uint8_t>& bytes, std::string* error) override {
        bytes.clear();
        auto fail = [&] { bytes.clear(); if (error) *error = "the copied file changed, expired or could not be read"; return false; };
        std::lock_guard serial(read_mutex_);
        if (!Active() || index >= entries_.size() || entries_[index].directory || length > kMaxClipboardFileRead ||
            offset > entries_[index].size || length > entries_[index].size - offset) return fail();
        const auto& file = files_[index];
        HANDLE input = ReOpenFile(file.handle, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT);
        if (input == INVALID_HANDLE_VALUE) return fail();
        struct Closer { HANDLE handle; ~Closer() { CloseHandle(handle); } } close{input};
        if (!SameFile(input, file.info)) return fail();
        OVERLAPPED operation{};
        operation.Offset = static_cast<DWORD>(offset); operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return fail();
        Closer close_event{operation.hEvent};
        bytes.resize(length);
        DWORD got = 0;
        bool started;
        {
            std::lock_guard lock(io_mutex_);
            if (!Active()) return fail();
            reading_ = input;
            started = ReadFile(input, bytes.data(), length, &got, &operation) != FALSE || GetLastError() == ERROR_IO_PENDING;
        }
        bool ok = false;
        if (started) {
            const DWORD wait = WaitForSingleObject(operation.hEvent, 10000);
            if (wait != WAIT_OBJECT_0) CancelIoEx(input, &operation);
            // The OVERLAPPED storage stays alive until completion even after cancel.
            ok = GetOverlappedResult(input, &operation, &got, TRUE) && wait == WAIT_OBJECT_0 && got == length;
        }
        { std::lock_guard lock(io_mutex_); reading_ = INVALID_HANDLE_VALUE; }
        if (!ok || !Active() || !SameFile(input, file.info)) return fail();
        return true;
    }
    bool Unchanged(size_t index) const override {
        if (!Active() || index >= entries_.size() || entries_[index].directory) return false;
        const auto& file = files_[index];
        HANDLE input = ReOpenFile(file.handle, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OPEN_REPARSE_POINT);
        if (input == INVALID_HANDLE_VALUE) return false;
        const bool same = SameFile(input, file.info);
        CloseHandle(input);
        return same;
    }
private:
    static bool SameFile(HANDLE handle, const BY_HANDLE_FILE_INFORMATION& expected) {
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(handle, &info) &&
            info.nFileSizeHigh == expected.nFileSizeHigh && info.nFileSizeLow == expected.nFileSizeLow &&
            CompareFileTime(&info.ftLastWriteTime, &expected.ftLastWriteTime) == 0 &&
            info.dwVolumeSerialNumber == expected.dwVolumeSerialNumber &&
            info.nFileIndexHigh == expected.nFileIndexHigh && info.nFileIndexLow == expected.nFileIndexLow;
    }
    struct File { HANDLE handle = INVALID_HANDLE_VALUE; BY_HANDLE_FILE_INFORMATION info{}; };
    std::vector<ClipboardFileEntry> entries_;
    std::vector<File> files_;
    std::atomic<bool> active_{true};
    uint64_t version_ = 14695981039346656037ull;
    std::mutex read_mutex_, io_mutex_;
    HANDLE reading_ = INVALID_HANDLE_VALUE;
};
}

std::shared_ptr<ClipboardVirtualFiles> make_local_clipboard_files(
    const std::vector<ClipboardFileEntry>& entries, const std::vector<std::string>& paths, std::string* error) {
    auto files = std::make_shared<LocalFiles>(entries);
    try { if (files->Open(paths)) return files; } catch (...) {}
    if (error) *error = "cannot retain the copied file identities";
    return {};
}
}
