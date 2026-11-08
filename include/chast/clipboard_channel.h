#pragma once

#include "chast/clipboard_virtual.h"
#include <memory>

namespace chast {
class QuicSession;

// Session-thread pump with a separate disk reader. COM readers keep only the
// remote offer's shared state, never a raw QuicSession or host/viewer pointer.
class ClipboardFileChannel {
public:
    ClipboardFileChannel();
    ~ClipboardFileChannel();
    void SetLocal(uint64_t generation, std::shared_ptr<ClipboardVirtualFiles> files);
    void ClearLocal();
    std::shared_ptr<ClipboardVirtualFiles> Receive(uint64_t generation,
                                                 std::vector<ClipboardFileEntry> entries);
    void CancelRemote();
    void Poll(QuicSession& session, bool sending, bool receiving);
private:
    struct State;
    class Remote;
    std::shared_ptr<State> state_;
    std::shared_ptr<ClipboardVirtualFiles> local_;
    std::shared_ptr<Remote> remote_;
    uint64_t local_generation_ = 0;
    uint64_t forget_generation_ = 0;
    uint32_t next_request_ = 1;
};
}
