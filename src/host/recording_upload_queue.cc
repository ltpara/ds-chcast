#define NOMINMAX

#include "chast/host_recording_upload.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

#include <openssl/sha.h>

namespace chast {
namespace {
namespace fs = std::filesystem;

fs::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (unsigned char byte : value) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return fs::path(encoded);
}

std::string path_to_utf8(const fs::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

std::string json_string(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (unsigned char byte : value) {
        switch (byte) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (byte < 0x20) {
                escaped += "\\u00";
                escaped.push_back(hex[byte >> 4]);
                escaped.push_back(hex[byte & 0x0f]);
            } else {
                escaped.push_back(static_cast<char>(byte));
            }
        }
    }
    return escaped;
}

bool make_task_id(const fs::path& source, const std::string& source_utf8,
                  uintmax_t size, const HostRecordingSegment& segment,
                  std::string& id) {
    HANDLE file = CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool read_identity = GetFileInformationByHandle(file, &info) != FALSE;
    CloseHandle(file);
    if (!read_identity) return false;

    std::string identity = "chast-host-recording-upload-task-v1\0";
    identity += source_utf8;
    identity.push_back('\0');
    const auto append_u32 = [&identity](uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            identity.push_back(static_cast<char>((value >> shift) & 0xff));
        }
    };
    const auto append_u64 = [&identity](uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            identity.push_back(static_cast<char>((value >> shift) & 0xff));
        }
    };
    append_u32(info.dwVolumeSerialNumber);
    append_u64((static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
    append_u64((static_cast<uint64_t>(info.ftCreationTime.dwHighDateTime) << 32) |
               info.ftCreationTime.dwLowDateTime);
    append_u64((static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
               info.ftLastWriteTime.dwLowDateTime);
    append_u64(size);
    append_u64(segment.started_at_ms);
    append_u64(segment.duration_ms);

    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(reinterpret_cast<const unsigned char*>(identity.data()), identity.size(),
               digest.data()) == nullptr) {
        return false;
    }
    static constexpr char hex[] = "0123456789abcdef";
    id.clear();
    id.reserve(32);
    for (size_t i = 0; i < 16; ++i) {
        id.push_back(hex[digest[i] >> 4]);
        id.push_back(hex[digest[i] & 0x0f]);
    }
    return true;
}

bool existing_task_matches(const fs::path& path, const std::string& expected,
                           bool* exists) {
    *exists = false;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;
    if (ec || !fs::is_regular_file(status)) return false;
    *exists = true;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size != expected.size()) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    return contents == expected;
}

bool write_durable_file(const fs::path& path, const std::string& contents,
                        DWORD* failure) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *failure = GetLastError();
        return false;
    }
    bool ok = true;
    size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD request = static_cast<DWORD>(std::min<size_t>(
            contents.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, request, &written, nullptr) || written == 0) {
            *failure = GetLastError();
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) {
        *failure = GetLastError();
        ok = false;
    }
    if (!CloseHandle(file) && ok) {
        *failure = GetLastError();
        ok = false;
    }
    return ok;
}

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}
}  // namespace

bool enqueue_host_recording_upload(const std::string& queue_directory,
                                   const HostRecordingSegment& segment,
                                   std::string* error) {
    if (queue_directory.empty() || queue_directory.find('\0') != std::string::npos ||
        segment.path.empty() || segment.path.find('\0') != std::string::npos ||
        segment.started_at_ms == 0 ||
        segment.started_at_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        segment.duration_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return fail(error, "recording upload task metadata is invalid");
    }

    fs::path queue;
    fs::path source;
    try {
        queue = path_from_utf8(queue_directory);
        source = path_from_utf8(segment.path);
    } catch (const std::exception& exception) {
        return fail(error, std::string("recording upload path is invalid: ") + exception.what());
    }
    if (!queue.is_absolute() || !source.is_absolute()) {
        return fail(error, "recording upload paths must be absolute");
    }

    std::error_code ec;
    const fs::file_status status = fs::symlink_status(source, ec);
    if (ec || !fs::is_regular_file(status)) {
        return fail(error, "sealed recording is not a regular file");
    }
    const uintmax_t size = fs::file_size(source, ec);
    if (ec || size == 0 || size > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max())) {
        return fail(error, "sealed recording has an invalid size");
    }
    std::string name;
    try {
        name = path_to_utf8(source.filename());
    } catch (const std::exception& exception) {
        return fail(error, std::string("recording name is invalid: ") + exception.what());
    }
    if (name.empty() || name == "." || name == "..") {
        return fail(error, "sealed recording has an invalid file name");
    }
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".mp4") {
        return fail(error, "sealed recording must be an MP4 file");
    }
    fs::create_directories(queue, ec);
    if (ec || !fs::is_directory(queue, ec) || ec) {
        return fail(error, "could not create the recording upload queue directory");
    }

    const std::string source_utf8 = path_to_utf8(source);
    std::string id;
    if (!make_task_id(source, source_utf8, size, segment, id)) {
        return fail(error, "could not derive a stable recording upload task id");
    }
    const std::string contents =
        "{\"id\":\"" + id + "\",\"path\":\"" + json_string(source_utf8) +
        "\",\"name\":\"" + json_string(name) + "\",\"size\":" +
        std::to_string(size) + ",\"started_at_ms\":" +
        std::to_string(segment.started_at_ms) + ",\"duration_ms\":" +
        std::to_string(segment.duration_ms) + "}\n";
    const fs::path destination = queue / path_from_utf8(id + ".json");
    bool already_exists = false;
    if (!existing_task_matches(destination, contents, &already_exists)) {
        return fail(error, "an existing recording upload task has conflicting contents");
    }
    if (already_exists) {
        if (error != nullptr) error->clear();
        return true;
    }

    static std::atomic<uint64_t> temporary_sequence{0};
    const std::string temporary_name = id + "." +
        std::to_string(GetCurrentProcessId()) + "." +
        std::to_string(++temporary_sequence) + ".tmp";
    const fs::path temporary = queue / path_from_utf8(temporary_name);
    DWORD failure = ERROR_SUCCESS;
    if (!write_durable_file(temporary, contents, &failure)) {
        return fail(error, "could not write recording upload task (error " +
                               std::to_string(failure) + ")");
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (error != nullptr) error->clear();
        return true;
    }
    failure = GetLastError();
    DeleteFileW(temporary.c_str());
    if (failure == ERROR_FILE_EXISTS || failure == ERROR_ALREADY_EXISTS) {
        already_exists = false;
        if (existing_task_matches(destination, contents, &already_exists) && already_exists) {
            if (error != nullptr) error->clear();
            return true;
        }
    }
    return fail(error, "could not publish recording upload task (error " +
                           std::to_string(failure) + ")");
}

}  // namespace chast
