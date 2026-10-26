// File receive semantics: name sanitizing, digest checking, abort/resume and
// final rename. Uses the process temp directory only.

#include <windows.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "chast/file_transfer.h"
#include "chast/security.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

std::array<uint8_t, 32> digest_of(const std::string& text) {
    std::array<uint8_t, 32> digest{};
    chast::sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest);
    return digest;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void test_sanitize() {
    check(chast::sanitize_received_name("report.pdf") == "report.pdf",
          "a plain name is kept");
    check(chast::sanitize_received_name("../../evil.txt") == "evil.txt",
          "path components are stripped");
    check(chast::sanitize_received_name("C:\\Windows\\system.ini") == "system.ini",
          "drive and directory components are stripped");
    check(chast::sanitize_received_name("con.txt") == "_con.txt",
          "a reserved device name is escaped even with an extension");
    check(chast::sanitize_received_name("COM7") == "_COM7",
          "a numbered reserved device name is escaped");
    check(chast::sanitize_received_name("a:b?.txt") == "a_b_.txt",
          "invalid characters are replaced");
    check(chast::sanitize_received_name("trailing. . .") == "trailing",
          "trailing dots and spaces are removed");
    check(chast::sanitize_received_name("   ") == "file",
          "a blank name falls back to file");
    check(chast::sanitize_received_name("") == "file", "an empty name falls back to file");
    const std::string long_name =
        chast::sanitize_received_name(std::string(400, 'a') + ".bin");
    check(long_name.size() <= chast::kMaxReceiveNameChars * 4 + 4 &&
              long_name.rfind(".bin") == long_name.size() - 4,
          "an over-long name keeps its extension inside the limit");
}

void test_sha256_file(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / "digest.txt";
    {
        std::ofstream output(path, std::ios::binary);
        output << "abc";
    }
    uint64_t size = 0;
    std::array<uint8_t, 32> digest{};
    std::string error;
    check(chast::sha256_file(path.string(), size, digest, &error) && size == 3,
          "sha256_file reports the size");
    // The first bytes of SHA-256("abc").
    check(digest[0] == 0xba && digest[1] == 0x78 && digest[2] == 0x16 &&
              digest[3] == 0xbf,
          "sha256_file matches the published SHA-256 of abc");
}

void test_receiver(const std::filesystem::path& directory) {
    const std::string content = "abcdef";
    chast::FileReceiveOptions options;
    options.directory = directory.string();
    options.name = "resume.bin";
    options.size = content.size();
    options.sha256 = digest_of(content);
    options.allow_resume = false;

    uint64_t offset = 99;
    std::string error;
    std::unique_ptr<chast::FileReceiver> receiver =
        chast::FileReceiver::create(options, &offset, &error);
    check(receiver != nullptr && offset == 0, "a fresh receiver starts at zero");
    if (receiver) {
        check(receiver->write(reinterpret_cast<const uint8_t*>(content.data()), 3, &error),
              "the first half is written");
        receiver->abort(true);
        check(std::filesystem::exists(directory / "resume.bin.part") &&
                  std::filesystem::exists(directory / "resume.bin.part.chast"),
              "abort(true) keeps the part file and its sidecar");
    }

    options.allow_resume = true;
    offset = 0;
    receiver = chast::FileReceiver::create(options, &offset, &error);
    check(receiver != nullptr && offset == 3, "a matching sidecar resumes at the part size");
    if (receiver) {
        check(receiver->write(reinterpret_cast<const uint8_t*>(content.data()) + 3, 3, &error),
              "the remainder is written");
        std::string final_path;
        check(receiver->finish(final_path, &error),
              "the completed file passes its digest and is renamed");
        check(read_file(final_path) == content, "the final file holds every byte");
        check(!std::filesystem::exists(directory / "resume.bin.part") &&
                  !std::filesystem::exists(directory / "resume.bin.part.chast"),
              "the part file and sidecar are gone after the rename");
        uint64_t size = 0;
        std::array<uint8_t, 32> digest{};
        check(chast::sha256_file(final_path, size, digest, &error) &&
                  size == content.size() && digest == options.sha256,
              "the final file hashes to the offered digest");
    }

    options.name = "wrong.bin";
    options.sha256.fill(0x5A);
    options.allow_resume = false;
    offset = 0;
    receiver = chast::FileReceiver::create(options, &offset, &error);
    check(receiver != nullptr, "a receiver with the wrong digest still starts");
    if (receiver) {
        check(receiver->write(reinterpret_cast<const uint8_t*>(content.data()), content.size(),
                              &error),
              "the bytes are accepted");
        std::string final_path;
        check(!receiver->finish(final_path, &error),
              "a digest mismatch fails the transfer");
        check(!std::filesystem::exists(directory / "wrong.bin.part") &&
                  !std::filesystem::exists(directory / "wrong.bin.part.chast"),
              "a digest mismatch removes the part file and sidecar");
    }
}

void test_sanitize_directory() {
    std::string path;
    check(chast::sanitize_received_directory("", path) && path.empty(),
          "an empty relative directory means the receive root");
    check(chast::sanitize_received_directory("Trip/2024", path) && path == "Trip/2024",
          "a plain relative directory is kept");
    check(chast::sanitize_received_directory("CON/a?b", path) && path == "_CON/a_b",
          "each directory component is sanitized like a file name");
    check(!chast::sanitize_received_directory("/absolute", path),
          "an absolute directory is rejected");
    check(!chast::sanitize_received_directory("C:/Windows", path),
          "a drive-letter directory is rejected");
    check(!chast::sanitize_received_directory("a/../b", path),
          "a .. component is rejected");
    check(!chast::sanitize_received_directory("a/./b", path),
          "a . component is rejected");
    check(!chast::sanitize_received_directory("a//b", path),
          "an empty component is rejected");
    check(!chast::sanitize_received_directory("a\\b", path),
          "a backslash is rejected");
    std::string deep;
    for (size_t i = 0; i <= chast::kMaxReceivePathDepth; ++i) {
        if (!deep.empty()) {
            deep.push_back('/');
        }
        deep.push_back('a');
    }
    check(!chast::sanitize_received_directory(deep, path),
          "a directory chain past the depth limit is rejected");
}

void test_nested_receiver(const std::filesystem::path& directory) {
    const std::string content = "nested-content";
    chast::FileReceiveOptions options;
    options.directory = directory.string();
    options.name = "report.txt";
    options.relative_path = "Trip/2024";
    options.size = content.size();
    options.sha256 = digest_of(content);

    uint64_t offset = 99;
    std::string error;
    std::unique_ptr<chast::FileReceiver> receiver =
        chast::FileReceiver::create(options, &offset, &error);
    check(receiver != nullptr && offset == 0,
          "a nested receiver starts at zero and creates its directory chain");
    check(std::filesystem::is_directory(directory / "Trip" / "2024"),
          "the relative directory exists below the receive root");
    if (receiver) {
        check(receiver->write(reinterpret_cast<const uint8_t*>(content.data()),
                              content.size(), &error),
              "the nested file is written");
        std::string final_path;
        check(receiver->finish(final_path, &error),
              "the nested file passes its digest and is renamed");
        check(std::filesystem::path(final_path) ==
                      directory / "Trip" / "2024" / "report.txt" &&
                  read_file(final_path) == content,
              "the final file lands in the nested directory");
    }

    offset = 0;
    receiver = chast::FileReceiver::create(options, &offset, &error);
    check(receiver != nullptr, "a second receiver accepts the same nested offer");
    if (receiver) {
        check(receiver->write(reinterpret_cast<const uint8_t*>(content.data()),
                              content.size(), &error),
              "the duplicate is written");
        std::string final_path;
        check(receiver->finish(final_path, &error) &&
                  std::filesystem::path(final_path).filename() == L"report (1).txt",
              "a duplicate leaf inside the nested directory gets a unique name");
    }

    options.name = "resume.txt";
    options.allow_resume = true;
    offset = 0;
    receiver = chast::FileReceiver::create(options, &offset, &error);
    check(receiver != nullptr, "a nested resumable offer starts");
    if (receiver) {
        check(receiver->write(reinterpret_cast<const uint8_t*>(content.data()), 7, &error),
              "the first part of the nested resumable file is written");
        receiver->abort(true);
    }
    offset = 0;
    receiver = chast::FileReceiver::create(options, &offset, &error);
    check(receiver != nullptr && offset == 7,
          "the nested resume continues from the .part length");
    if (receiver) {
        check(receiver->write(reinterpret_cast<const uint8_t*>(content.data()) + 7,
                              content.size() - 7, &error),
              "the remainder of the nested resumable file is written");
        std::string final_path;
        check(receiver->finish(final_path, &error) &&
                  std::filesystem::path(final_path) ==
                      directory / "Trip" / "2024" / "resume.txt",
              "the resumed nested file is finalized in place");
    }

    chast::FileReceiveOptions escaped = options;
    escaped.relative_path = "../escape";
    escaped.allow_resume = false;
    offset = 0;
    receiver = chast::FileReceiver::create(escaped, &offset, &error);
    check(receiver == nullptr &&
              !std::filesystem::exists(directory.parent_path() / "escape"),
          "a traversal in the relative directory is refused without creating anything");

    {
        std::ofstream blocker(directory / "blocker", std::ios::binary);
        blocker << "x";
    }
    chast::FileReceiveOptions crossing = options;
    crossing.relative_path = "blocker";
    crossing.name = "inside.txt";
    crossing.allow_resume = false;
    offset = 0;
    receiver = chast::FileReceiver::create(crossing, &offset, &error);
    check(receiver == nullptr, "a directory component that is a file is refused");
}

}  // namespace

int main() {
    wchar_t temp[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    std::filesystem::path directory;
    if (length > 0) {
        directory = std::filesystem::path(temp) /
                    ("chast-file-transfer-test-" + std::to_string(GetCurrentProcessId()));
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);

    test_sanitize();
    test_sanitize_directory();
    if (!directory.empty()) {
        test_sha256_file(directory);
        test_receiver(directory);
        test_nested_receiver(directory);
    } else {
        std::printf("FAIL could not create a temp directory\n");
        ++failures;
    }
    std::filesystem::remove_all(directory, ignored);

    if (failures != 0) {
        std::printf("file transfer checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("file transfer checks passed\n");
    return 0;
}
