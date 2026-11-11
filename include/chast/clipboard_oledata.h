#pragma once

// OLE virtual file sources.
//
// Some applications put a copy on the clipboard without real file paths: a
// CFSTR_FILEDESCRIPTORW list of names and sizes plus delayed
// CFSTR_FILECONTENTS streams rendered only when a paste target asks. The
// CF_HDROP reader in clipboard_win.cc cannot see those files, so this module
// turns the OLE offer into the same delayed ClipboardVirtualFiles source the
// path-based reader produces.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/clipboard.h"

struct IDataObject;

namespace chast {

class ClipboardVirtualFiles;

// Parses one CFSTR_FILEDESCRIPTORW block. Every entry is a flat base name;
// Windows puts no paths in this format. A regular file must carry FD_FILESIZE,
// a size within kMaxFileTransferBytes and a valid name; a directory carries no
// size and is never readable. `version` mixes the names, sizes, attributes and
// last-write times so a re-copy of the same names is announced again.
bool parse_ole_file_descriptors(const uint8_t* data, size_t size,
                                std::vector<ClipboardFileEntry>& entries,
                                uint64_t& version, std::string* error);

// Wraps an OLE data object as a delayed file source. The object is read on a
// dedicated thread with its own COM apartment; Read() may be called from any
// thread. Cancel() fails waiting and later reads without joining a blocked
// source call, and the reader releases its streams once it next wakes.
std::shared_ptr<ClipboardVirtualFiles> make_ole_clipboard_files(
    IDataObject* object, const std::vector<ClipboardFileEntry>& entries,
    uint64_t version, std::string* error);

// Reads the system clipboard's virtual file offer through OleGetClipboard.
// Empty means the clipboard holds no file descriptor; Content fills `entries`
// and `files`; Failed reports a malformed or unreadable offer. Requires OLE
// initialization on the calling thread. Our own published offer is skipped so
// a poll never forces a remote read.
ClipboardReadState read_ole_clipboard_files_state(
    std::vector<ClipboardFileEntry>& entries,
    std::shared_ptr<ClipboardVirtualFiles>& files, std::string* error);

}  // namespace chast
