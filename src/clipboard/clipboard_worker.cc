#include "chast/clipboard_worker.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace chast {
struct ClipboardWorker::State {
    struct Job {
        Operation operation;
        uint64_t epoch;
        uint64_t generation;
        ClipboardContent content;
        std::vector<std::string> paths;
        std::shared_ptr<ClipboardVirtualFiles> files;
    };
    ClipboardBackend backend;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopped = false;
    bool active = false;
    uint64_t generation = 0;
    std::optional<Job> job;
    std::optional<Result> result;
    std::shared_ptr<ClipboardVirtualFiles> published;
};

ClipboardWorker::ClipboardWorker(ClipboardBackend backend) : state_(std::make_shared<State>()) {
    state_->backend = std::move(backend);
    std::thread([state = state_] {
        std::unique_ptr<VirtualClipboard> native_files;
        std::unique_lock lock(state->mutex);
        while (true) {
            state->cv.wait(lock, [&] { return state->stopped || state->job.has_value(); });
            if (state->stopped) return;
            auto job = std::move(*state->job);
            state->job.reset();
            state->active = true;
            const auto backend = state->backend;
            lock.unlock();
            Result result;
            result.operation = job.operation;
            result.epoch = job.epoch;
            auto current = [&] {
                std::lock_guard guard(state->mutex);
                return !state->stopped && job.generation == state->generation;
            };
            try {
                if (job.operation == Operation::Read) {
                    auto& snapshot = result.snapshot;
                    std::vector<std::string> found;
                    const auto files = backend.read_files ? backend.read_files(found)
                        : read_clipboard_files_state(found, &snapshot.error);
                    if (current() && files == ClipboardReadState::Content) {
                        snapshot.has_files = collect_clipboard_files(found, snapshot.entries,
                                                                     snapshot.paths, &snapshot.error);
                        snapshot.state = snapshot.has_files ? ClipboardReadState::Content
                                                            : ClipboardReadState::Failed;
                        if (snapshot.has_files) {
                            snapshot.local_files = make_local_clipboard_files(snapshot.entries, snapshot.paths, &snapshot.error);
                            if (!snapshot.local_files) snapshot.state = ClipboardReadState::Failed;
                        }
                    } else if (current() && files == ClipboardReadState::Empty) {
                        snapshot.state = backend.read ? backend.read(snapshot.content)
                            : read_clipboard_state(snapshot.content, &snapshot.error);
                    }
                } else if (!current()) {
                    if (job.files) job.files->Cancel();
                } else if (job.operation == Operation::WriteVirtualFiles) {
                    if (backend.write_virtual_files) result.written = backend.write_virtual_files(job.files);
                    else {
                        if (!native_files) native_files = std::make_unique<VirtualClipboard>();
                        result.written = native_files->Publish(job.files, &result.snapshot.error);
                    }
                    if (!current() && job.files) job.files->Cancel();
                } else if (job.operation == Operation::Write) {
                    result.written = backend.write ? backend.write(job.content)
                        : write_clipboard(job.content, &result.snapshot.error);
                } else {
                    result.written = backend.write_files ? backend.write_files(job.paths)
                        : write_clipboard_files(job.paths, &result.snapshot.error);
                }
            } catch (...) {
                result.snapshot.state = ClipboardReadState::Failed;
                result.snapshot.error = "clipboard backend failed";
            }
            lock.lock();
            state->active = false;
            if (!state->stopped && job.generation == state->generation) {
                state->result = std::move(result);
            }
        }
    }).detach();
}

ClipboardWorker::~ClipboardWorker() {
    std::lock_guard lock(state_->mutex);
    state_->stopped = true;
    if (state_->published) state_->published->Cancel();
    state_->job.reset();
    state_->result.reset();
    state_->cv.notify_one();
}

void ClipboardWorker::Cancel() {
    std::lock_guard lock(state_->mutex);
    if (state_->published) state_->published->Cancel();
    state_->published.reset();
    ++state_->generation;
    state_->job.reset();
    state_->result.reset();
}

void ClipboardWorker::SetBackend(ClipboardBackend backend) {
    std::lock_guard lock(state_->mutex);
    if (state_->published) state_->published->Cancel();
    state_->published.reset();
    ++state_->generation;
    state_->job.reset();
    state_->result.reset();
    state_->backend = std::move(backend);
}

bool ClipboardWorker::Submit(Operation operation, uint64_t epoch,
                             const ClipboardContent& content, const std::vector<std::string>& paths,
                             std::shared_ptr<ClipboardVirtualFiles> files) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || state_->active || state_->job || state_->result) return false;
    if (files) state_->published = files;
    state_->job = State::Job{operation, epoch, state_->generation,
                            content, paths, std::move(files)};
    state_->cv.notify_one();
    return true;
}

bool ClipboardWorker::Read(uint64_t epoch) { return Submit(Operation::Read, epoch, {}, {}); }
bool ClipboardWorker::Write(uint64_t epoch, const ClipboardContent& content) {
    return Submit(Operation::Write, epoch, content, {});
}
bool ClipboardWorker::WriteFiles(uint64_t epoch, const std::vector<std::string>& paths) {
    return Submit(Operation::WriteFiles, epoch, {}, paths);
}
bool ClipboardWorker::WriteVirtualFiles(uint64_t epoch, std::shared_ptr<ClipboardVirtualFiles> files) {
    return Submit(Operation::WriteVirtualFiles, epoch, {}, {}, std::move(files));
}
bool ClipboardWorker::Take(Result& result) {
    std::lock_guard lock(state_->mutex);
    if (!state_->result) return false;
    result = std::move(*state_->result);
    state_->result.reset();
    return true;
}
} // namespace chast
