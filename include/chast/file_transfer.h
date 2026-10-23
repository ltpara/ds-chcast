#pragma once

// Host-side file reception: name sanitizing, unique naming, resumable .part
// files and digest verification. The viewer uses sha256_file() before it sends
// a FileOffer; everything else here runs on the host.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace chast {

// The sanitized name is limited to this many UTF-16 units (Windows counts
// path components in UTF-16, and 200 leaves room for a " (n)" suffix and the
// .part marker inside a 255-unit component).
inline constexpr size_t kMaxReceiveNameChars = 200;
// Refuse an offer that would leave less than this on the receive volume, so
// completing the transfer never fills the disk.
inline constexpr uint64_t kMinReceiveFreeSpace = 64ull * 1024ull * 1024ull;

// Makes a peer-supplied name safe to create inside one directory: takes the
// last path component, replaces control characters and the Windows-invalid
// set, strips trailing dots and spaces, avoids the reserved DOS device names
// and returns "file" when nothing usable remains. The extension is kept.
std::string sanitize_received_name(const std::string& utf8_name);

// %USERPROFILE%\Downloads\chast, or an empty string when the shell cannot name
// the Downloads folder. Hosts use it when no receive directory is configured.
std::string default_receive_directory();

struct FileReceiveOptions {
    // UTF-8 directory; created when missing.
    std::string directory;
    // Raw peer name; sanitized by create().
    std::string name;
    uint64_t size = 0;
    std::array<uint8_t, 32> sha256{};
    // Honour an existing .part + sidecar for the same offer.
    bool allow_resume = false;
};

class FileReceiver {
public:
    // Opens the receive directory and prepares the .part file. On success
    // `resume_offset` is the byte the sender should start at. Returns nullptr
    // with `error` for an unusable name, insufficient disk space, a reparse
    // point or a file-system failure.
    static std::unique_ptr<FileReceiver> create(const FileReceiveOptions& options,
                                                uint64_t* resume_offset, std::string* error);
    ~FileReceiver();
    FileReceiver(const FileReceiver&) = delete;
    FileReceiver& operator=(const FileReceiver&) = delete;

    // Appends `size` bytes at the current receive position. Never crosses the
    // offered total; on a short write the receiver is unusable and abort()
    // must follow.
    bool write(const uint8_t* data, size_t size, std::string* error);

    // Checks the length and SHA-256, flushes, then renames the .part to its
    // final name. A digest mismatch removes the .part and sidecar. A rename
    // failure keeps them for a retry.
    bool finish(std::string& final_path, std::string* error);

    // Stops without finishing. keep_partial leaves .part and .part.chast so a
    // later offer can resume; otherwise both are deleted.
    void abort(bool keep_partial);

    uint64_t received() const;
    uint64_t size() const;
    // Sanitized name without the uniqueness suffix.
    const std::string& name() const;

private:
    FileReceiver();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whole-file SHA-256 and size, for FileOffer. Shares read access so a file
// another process is writing can still be hashed; the caller is responsible
// for not offering a file that changes while it is read.
bool sha256_file(const std::string& utf8_path, uint64_t& size,
                 std::array<uint8_t, 32>& digest, std::string* error);

}  // namespace chast
