#pragma once

#include <functional>
#include <memory>
#include "chast/clipboard.h"

namespace chast {

struct ClipboardBackend {
    // Callbacks run on a worker and must own the state they capture. They may
    // finish after the endpoint is destroyed. Empty callbacks use Win32.
    std::function<ClipboardReadState(ClipboardContent&)> read;
    std::function<ClipboardReadState(std::vector<std::string>&)> read_files;
    std::function<bool(const ClipboardContent&)> write;
    std::function<bool(const std::vector<std::string>&)> write_files;
};

struct ClipboardSnapshot {
    ClipboardReadState state = ClipboardReadState::Failed;
    bool has_files = false;
    ClipboardContent content;
    std::vector<ClipboardFileEntry> entries;
    std::vector<std::string> paths;
    std::string error;
};

// One persistent executor serializes all clipboard I/O for an endpoint. It
// owns its callbacks independently of that endpoint; cancellation discards
// queued work/results without joining an uninterruptible Windows read.
class ClipboardWorker {
public:
    enum class Operation { Read, Write, WriteFiles };
    struct Result {
        Operation operation = Operation::Read;
        uint64_t epoch = 0;
        ClipboardSnapshot snapshot;
        bool written = false;
    };
    explicit ClipboardWorker(ClipboardBackend backend);
    ~ClipboardWorker();
    ClipboardWorker(const ClipboardWorker&) = delete;
    ClipboardWorker& operator=(const ClipboardWorker&) = delete;
    bool Read(uint64_t epoch);
    bool Write(uint64_t epoch, const ClipboardContent& content);
    bool WriteFiles(uint64_t epoch, const std::vector<std::string>& paths);
    bool Take(Result& result);
    void Cancel();
    void SetBackend(ClipboardBackend backend);

private:
    struct State;
    std::shared_ptr<State> state_;
    bool Submit(Operation operation, uint64_t epoch, const ClipboardContent& content,
                const std::vector<std::string>& paths);
};

} // namespace chast
