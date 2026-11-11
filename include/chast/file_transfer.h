#pragma once

// Host-side file reception: name sanitizing, unique naming, resumable .part
// files and digest verification. The viewer uses sha256_file() before it sends
// a FileOffer; everything else here runs on the host.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/protocol.h"

namespace chast {

// The sanitized name is limited to this many UTF-16 units (Windows counts
// path components in UTF-16, and 200 leaves room for a " (n)" suffix and the
// .part marker inside a 255-unit component).
inline constexpr size_t kMaxReceiveNameChars = 200;
// Refuse an offer that would leave less than this on the receive volume, so
// completing the transfer never fills the disk.
inline constexpr uint64_t kMinReceiveFreeSpace = 64ull * 1024ull * 1024ull;
// Deepest directory chain a single offer may create below the receive root.
inline constexpr size_t kMaxReceivePathDepth = 32;

// Makes a peer-supplied name safe to create inside one directory: takes the
// last path component, replaces control characters and the Windows-invalid
// set, strips trailing dots and spaces, avoids the reserved DOS device names
// and returns "file" when nothing usable remains. The extension is kept.
std::string sanitize_received_name(const std::string& utf8_name);

// Sanitizes the directory part of an offer ('/'-separated, no file name) the
// same way as a name, one component at a time. Empty means the receive root.
// Returns false for absolute paths, drive letters, "." / "..", empty
// components, over-long paths or more than kMaxReceivePathDepth components.
bool sanitize_received_directory(const std::string& utf8_path, std::string& sanitized);

// Create a metadata-only directory below an existing receive root. Rejects
// paths that would be renamed by sanitization, and reparse points in the
// chain; useful for clipboard empty folders, which have no FileReceiver.
bool create_receive_directory(const std::string& root, const std::string& relative,
                              std::string* error);

// %USERPROFILE%\Downloads\chast, or an empty string when the shell cannot name
// the Downloads folder. Hosts use it when no receive directory is configured.
std::string default_receive_directory();

struct FileReceiveOptions {
    // UTF-8 directory; created when missing.
    std::string directory;
    // Raw peer name; sanitized by create().
    std::string name;
    // Optional '/'-separated directory below `directory`; sanitized and
    // created by create(). Empty places the file in `directory` itself.
    std::string relative_path;
    uint64_t size = 0;
    std::array<uint8_t, 32> sha256{};
    // Honour an existing .part + sidecar for the same offer.
    bool allow_resume = false;
    // Replace an existing file at the destination. The receiver never renames
    // around a conflict on its own; the caller resolves it first.
    bool overwrite = false;
};

// What already holds an offer's destination name. The receiver inspects the
// target before answering so it can ask its user instead of renaming silently.
struct FileReceiveTarget {
    bool exists = false;
    bool directory = false;
    uint64_t size = 0;
    uint64_t modified_time = 0;  // Unix seconds, 0 when unknown
};

// Reports whether the offer's sanitized destination already exists below its
// receive directory. Nothing is created and no reparse point is followed; a
// missing directory chain means the destination does not exist yet. Returns
// false with `error` only when the name, path or directory is unusable.
bool inspect_receive_target(const FileReceiveOptions& options, FileReceiveTarget& target,
                            std::string* error);

class FileReceiver {
public:
    // Opens the receive directory and prepares the .part file. On success
    // `resume_offset` is the byte the sender should start at. Returns nullptr
    // with `error` when the destination already exists and
    // `options.overwrite` is false, when it is a directory, or for an
    // unusable name, insufficient disk space, a reparse point or a
    // file-system failure.
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
    // Sanitized name the file is written as.
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

// ----- shared-folder browsing -----
//
// The viewer browses and downloads from the same receive root that receives
// uploads. Paths are '/'-separated relative paths; every component is resolved
// with GetFileAttributesW and a reparse point anywhere on the way refuses the
// request, so a junction cannot widen the sandbox.

// Resolves `relative` below `root`. Ok fills `full_path` (absolute) and
// `is_directory`; NotFound means a component does not exist, Refused an
// invalid or escaping path, IoError a file-system failure. `error` carries a
// UTF-8 explanation for every code other than Ok.
FileCode resolve_receive_path(const std::string& root, const std::string& relative,
                              std::string& full_path, bool& is_directory,
                              std::string& error);

// Fills `result.entries`, `result.truncated`, `result.next_cursor`, `result.code` and
// `result.reason` for one browse request; the caller sets `request_id`.
// Directories come first, then files, each sorted by name. Reparse points and
// names too long or not representable as UTF-8 are skipped, and at most
// kMaxFileListEntries entries are returned with `truncated` set when more
// exist. `cursor` continues strictly after the previous page's final entry.
// Entries with FILE_ATTRIBUTE_HIDDEN are skipped unless `show_hidden`, before
// paging, so a cursor names an entry of the same visible order.
// Enumeration keeps at most one page plus one lookahead entry in memory.
void list_receive_directory(const std::string& root, const std::string& relative,
                            FileListResult& result, std::string cursor = {},
                            bool show_hidden = false);

// Applies one viewer file operation below `root`: create a folder, rename an
// entry inside its current folder, or remove a file / empty directory.
// `path` follows the browse rules and may not be empty; `new_name` is one
// component for Rename and empty otherwise. Ok means the change happened;
// Refused covers policy, escaping paths, reparse points, occupied names,
// non-empty folders and unusable names; NotFound and IoError carry the usual
// meanings. `error` explains every code other than Ok.
FileCode apply_file_operation(const std::string& root, FileOperation op,
                              const std::string& path, const std::string& new_name,
                              std::string& error);

// ----- multiple shared roots -----
//
// Without configured shares the receive root is the whole sandbox. A host may
// instead share several absolute directories (drive roots included). The
// viewer then sees one virtual top-level directory per share, named after the
// configured folder, and every path below a share resolves inside that share
// with the same reparse-point and traversal rules.

// Upper bound on configured shares, so the virtual top level always fits in
// one listing page.
inline constexpr size_t kMaxFileShares = 16;

struct FileShareRoot {
    // Single safe path component the viewer sees and echoes back.
    std::string name;
    // Absolute UTF-8 directory.
    std::string path;
};

// Turns configured absolute paths into stable, unique, wire-safe share names.
// Empty entries, relative paths and duplicates are skipped; a drive root is
// named after its letter, other roots after their final component, and a
// collision gets a " (2)" suffix.
std::vector<FileShareRoot> make_file_share_roots(const std::vector<std::string>& configured);

// Fills a successful top-level listing with one directory entry per share.
// A non-empty cursor is refused: the top level never pages.
void list_file_share_roots(const std::vector<FileShareRoot>& shares, FileListResult& result,
                           const std::string& cursor = {});

// Splits a viewer path into the share it names and the path below that share.
// An empty `path` is refused; callers list the shares themselves. With no
// shares, `fallback_root` is used and `path` is returned unchanged. A path
// whose first component is not a configured share reports NotFound.
FileCode split_file_share_path(const std::vector<FileShareRoot>& shares,
                               const std::string& fallback_root, const std::string& path,
                               std::string& root, std::string& relative,
                               std::string& error);

}  // namespace chast
