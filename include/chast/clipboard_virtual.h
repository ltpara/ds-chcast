#pragma once

#include "chast/clipboard.h"

#include <memory>

struct IDataObject;

namespace chast {

// An immutable remote file offer. Readers can outlive their connection; Cancel
// must wake outstanding reads, and subsequent reads must fail. No local paths
// or endpoint pointers are exposed to the Windows clipboard owner.
class ClipboardVirtualFiles {
public:
    virtual ~ClipboardVirtualFiles() = default;
    virtual const std::vector<ClipboardFileEntry>& entries() const = 0;
    virtual bool Read(size_t index, uint64_t offset, uint32_t length,
                      std::vector<uint8_t>& bytes, std::string* error) = 0;
    virtual bool Active() const = 0;
    virtual uint64_t SourceVersion() const { return 0; }
    virtual void Cancel() = 0;
};

// Captures file identities with metadata handles on the clipboard worker.
// Opens a data handle only for a Read and rejects changed/replaced files.
std::shared_ptr<ClipboardVirtualFiles> make_local_clipboard_files(
    const std::vector<ClipboardFileEntry>& entries, const std::vector<std::string>& paths,
    std::string* error);

// Creates the native data object without replacing the system clipboard.
// The caller owns its initial COM reference.
IDataObject* make_virtual_file_data_object(std::shared_ptr<ClipboardVirtualFiles> files,
                                         std::string* error);

// Owns a message-pumping OLE apartment. Publishing descriptors does not read
// file bytes; Windows requests them through IStream only when needed. Destroying
// this wrapper cancels readers without waiting for another application's COM call.
class VirtualClipboard {
public:
    VirtualClipboard();
    ~VirtualClipboard();
    VirtualClipboard(const VirtualClipboard&) = delete;
    VirtualClipboard& operator=(const VirtualClipboard&) = delete;
    bool Publish(std::shared_ptr<ClipboardVirtualFiles> files, std::string* error);
    void Clear();

private:
    struct State;
    std::shared_ptr<State> state_;
};

// The native reader skips our own remote offers instead of forcing their
// delayed formats to render (or forwarding them back to another connection).
bool is_virtual_clipboard();

} // namespace chast
