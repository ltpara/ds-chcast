#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "chast/host_recording_upload.h"

namespace {
namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, const char* message) {
    std::cout << (condition ? "ok   " : "FAIL ") << message << '\n';
    if (!condition) ++failures;
}

std::string utf8(const fs::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

fs::path from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (unsigned char byte : value) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return fs::path(encoded);
}

std::string escape_path(const std::string& value) {
    std::string escaped;
    for (char c : value) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

void run() {
    const fs::path root = fs::temp_directory_path() /
        (L"chast-recording-upload-queue-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    const fs::path source_directory = root / L"recordings";
    const fs::path queue = root / L"queue";
    std::error_code ec;
    fs::create_directories(source_directory, ec);
    check(!ec, "isolated source directory is created");
    const fs::path source = source_directory / L"host clip.mp4";
    {
        std::ofstream file(source, std::ios::binary);
        file << "sealed!";
    }

    const chast::HostRecordingSegment segment{utf8(source), 1795400000123ull, 6789};
    std::string error;
    check(chast::enqueue_host_recording_upload(utf8(queue), segment, &error),
          "a sealed MP4 is atomically published to the queue");
    if (!error.empty()) std::cout << "     " << error << '\n';

    fs::path manifest;
    size_t manifests = 0;
    size_t temporary_files = 0;
    for (const auto& entry : fs::directory_iterator(queue, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = utf8(entry.path().filename());
        if (name.ends_with(".tmp.json")) ++temporary_files;
        if (name.ends_with(".json") && !name.ends_with(".tmp.json")) {
            ++manifests;
            manifest = entry.path();
        }
    }
    check(!ec && manifests == 1 && temporary_files == 0,
          "only the complete final manifest is visible");

    std::string contents;
    if (!manifest.empty()) {
        std::ifstream file(manifest, std::ios::binary);
        contents.assign(std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>());
    }
    const std::string id = manifest.stem().string();
    bool valid_id = id.size() == 32;
    for (unsigned char c : id) {
        valid_id = valid_id && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    check(valid_id && contents.find("\"id\":\"" + id + "\"") != std::string::npos,
          "manifest id is a stable-format 32-character lowercase hex key");
    check(contents.find("\"path\":\"" + escape_path(utf8(source)) + "\"") !=
              std::string::npos &&
              contents.find("\"name\":\"host clip.mp4\"") != std::string::npos,
          "manifest path and name match the sealed source file");
    check(contents.find("\"size\":7") != std::string::npos &&
              contents.find("\"started_at_ms\":1795400000123") != std::string::npos &&
              contents.find("\"duration_ms\":6789") != std::string::npos,
          "manifest preserves size and segment timing metadata");

    check(chast::enqueue_host_recording_upload(utf8(queue), segment, &error),
          "a repeated completion callback is idempotent");
    size_t repeated = 0;
    for (const auto& entry : fs::directory_iterator(queue, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == L".json") ++repeated;
    }
    check(!ec && repeated == 1, "repeated callbacks retain one stable task manifest");

    chast::HostRecordingSegment relative = segment;
    relative.path = "relative.mp4";
    check(!chast::enqueue_host_recording_upload(utf8(queue), relative, &error),
          "relative source paths are rejected");
    chast::HostRecordingSegment invalid_time = segment;
    invalid_time.started_at_ms = 0;
    check(!chast::enqueue_host_recording_upload(utf8(queue), invalid_time, &error),
          "a missing wall-clock start time is rejected");
    fs::remove_all(root, ec);
    check(!ec, "isolated queue files are removed after the test");
}

int emit_fixture(const std::string& queue_arg, uint64_t size) {
    const fs::path queue = fs::absolute(from_utf8(queue_arg));
    const fs::path source = queue.parent_path() / L"cpp-sealed-segment.mp4";
    std::error_code ec;
    fs::create_directories(queue, ec);
    if (ec) {
        std::cerr << "could not create fixture queue: " << ec.message() << '\n';
        return 2;
    }
    {
        std::ofstream file(source, std::ios::binary | std::ios::trunc);
        constexpr char pattern[] = "cpp-sealed-recording-segment-";
        uint64_t remaining = size;
        while (remaining > 0) {
            const auto length = static_cast<std::streamsize>(
                std::min<uint64_t>(remaining, sizeof(pattern) - 1));
            file.write(pattern, length);
            remaining -= static_cast<uint64_t>(length);
        }
        if (!file) {
            std::cerr << "could not write fixture recording\n";
            return 2;
        }
    }
    const chast::HostRecordingSegment segment{utf8(source), 1795400000123ull, 1337};
    std::string error;
    if (!chast::enqueue_host_recording_upload(utf8(queue), segment, &error)) {
        std::cerr << error << '\n';
        return 2;
    }
    std::cout << "source=" << utf8(source) << '\n';
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--emit-fixture") {
        return emit_fixture(argv[2], sizeof("cpp-sealed-recording-segment-") - 1);
    }
    if (argc == 4 && std::string(argv[1]) == "--emit-fixture") {
        try {
            return emit_fixture(argv[2], std::stoull(argv[3]));
        } catch (const std::exception& error) {
            std::cerr << "invalid fixture size: " << error.what() << '\n';
            return 2;
        }
    }
    run();
    if (failures != 0) {
        std::cout << "recording upload queue checks failed: " << failures << '\n';
        return 1;
    }
    std::cout << "recording upload queue checks passed\n";
    return 0;
}
