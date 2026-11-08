#include "chast/clipboard_channel.h"
#include "chast/quic.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace chast {
class ClipboardFileChannel::Remote final : public ClipboardVirtualFiles {
public:
    Remote(uint64_t generation, std::vector<ClipboardFileEntry> entries)
        : generation(generation), entries_(std::move(entries)) {}
    const std::vector<ClipboardFileEntry>& entries() const override { return entries_; }
    bool Active() const override { return active_.load(); }
    void Cancel() override {
        active_ = false;
        std::lock_guard lock(mutex_);
        waiting_.clear(); queued_.clear(); cv_.notify_all();
    }
    bool Read(size_t index, uint64_t offset, uint32_t length, std::vector<uint8_t>& bytes, std::string* error) override {
        bytes.clear();
        std::unique_lock lock(mutex_);
        auto fail = [&] { if (error) *error = "the remote clipboard read failed or was cancelled"; return false; };
        if (!Active() || index >= entries_.size() || entries_[index].directory || !length || length > kMaxClipboardFileRead ||
            offset > entries_[index].size || length > entries_[index].size - offset || waiting_.size() >= 8) return fail();
        auto pending = std::make_shared<Pending>();
        pending->request.generation = generation;
        pending->request.index = static_cast<uint32_t>(index);
        pending->request.offset = offset; pending->request.length = length;
        queued_.push_back(pending);
        waiting_.push_back(pending);
        const bool ready = cv_.wait_for(lock, std::chrono::seconds(20), [&] { return !Active() || pending->done; });
        if (!ready) { active_ = false; queued_.clear(); cv_.notify_all(); }
        std::erase(waiting_, pending);
        if (!Active() || !pending->done || !pending->response.ok || pending->response.bytes.size() != length) return fail();
        bytes = std::move(pending->response.bytes);
        return true;
    }
    bool Take(uint32_t id, ClipboardFileRead& request) {
        std::lock_guard lock(mutex_);
        if (!Active() || queued_.empty()) return false;
        auto pending = queued_.front(); queued_.pop_front();
        pending->request.request_id = id;
        request = pending->request;
        return true;
    }
    void Complete(ClipboardFileData response) {
        std::lock_guard lock(mutex_);
        if (!Active() || response.generation != generation) return;
        for (const auto& pending : waiting_) {
            if (pending->request.request_id == response.request_id && !pending->done) {
                pending->response = std::move(response); pending->done = true; cv_.notify_all(); break;
            }
        }
    }
    const uint64_t generation;
private:
    struct Pending { ClipboardFileRead request; ClipboardFileData response; bool done = false; };
    std::vector<ClipboardFileEntry> entries_;
    std::atomic<bool> active_{true};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Pending>> queued_;
    std::vector<std::shared_ptr<Pending>> waiting_;
};

struct ClipboardFileChannel::State {
    struct Job { ClipboardFileRead request; std::shared_ptr<ClipboardVirtualFiles> source; };
    struct Done { ClipboardFileData response; std::shared_ptr<ClipboardVirtualFiles> source; };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Done> results;
    bool stopped = false;
};

ClipboardFileChannel::ClipboardFileChannel() : state_(std::make_shared<State>()) {
    std::thread([state = state_] {
        std::unique_lock lock(state->mutex);
        for (;;) {
            state->cv.wait(lock, [&] { return state->stopped || !state->jobs.empty(); });
            if (state->stopped) return;
            auto job = std::move(state->jobs.front()); state->jobs.pop_front();
            lock.unlock();
            ClipboardFileData response{job.request.request_id, job.request.generation};
            try {
                response.ok = job.source->Active() && job.source->Read(job.request.index, job.request.offset,
                    job.request.length, response.bytes, nullptr) && job.source->Active();
            } catch (...) { response.ok = false; }
            if (!response.ok) response.bytes.clear();
            lock.lock();
            if (!state->stopped && state->results.size() < 8)
                state->results.push_back({std::move(response), std::move(job.source)});
        }
    }).detach();
}

ClipboardFileChannel::~ClipboardFileChannel() {
    ClearLocal(); CancelRemote();
    std::lock_guard lock(state_->mutex);
    state_->stopped = true; state_->jobs.clear(); state_->results.clear(); state_->cv.notify_all();
}

void ClipboardFileChannel::SetLocal(uint64_t generation, std::shared_ptr<ClipboardVirtualFiles> files) {
    ClearLocal(); local_generation_ = generation; local_ = std::move(files);
}
void ClipboardFileChannel::ClearLocal() {
    if (local_) local_->Cancel();
    local_.reset(); local_generation_ = 0;
}
void ClipboardFileChannel::CancelRemote() {
    if (remote_) { remote_->Cancel(); forget_generation_ = remote_->generation; }
    remote_.reset();
}
std::shared_ptr<ClipboardVirtualFiles> ClipboardFileChannel::Receive(uint64_t generation,
                                                                   std::vector<ClipboardFileEntry> entries) {
    CancelRemote();
    if (!generation || encode_clipboard_file_list(entries).empty()) return {};
    remote_ = std::make_shared<Remote>(generation, std::move(entries));
    return remote_;
}

void ClipboardFileChannel::Poll(QuicSession& session, bool sending, bool receiving) {
    if (!sending) ClearLocal();
    if (!receiving || (remote_ && !remote_->Active())) CancelRemote();
    if (forget_generation_) { session.cancel_clipboard_file_reads(forget_generation_); forget_generation_ = 0; }
    ClipboardFileData response;
    while (session.take_clipboard_file_data(response)) if (remote_) remote_->Complete(std::move(response));
    if (remote_) {
        ClipboardFileRead request;
        while (remote_->Take(next_request_, request)) {
            if (++next_request_ == 0) ++next_request_;
            if (!session.request_clipboard_file(request).ok()) remote_->Complete({request.request_id, request.generation});
        }
    }
    ClipboardFileRead request;
    while (session.take_clipboard_file_read(request)) {
        bool queued = false;
        if (sending && local_ && local_->Active() && request.generation == local_generation_) {
            std::lock_guard lock(state_->mutex);
            if (state_->jobs.size() + state_->results.size() < 8) {
                state_->jobs.push_back({request, local_}); state_->cv.notify_one(); queued = true;
            }
        }
        if (!queued) (void)session.send_clipboard_file_data({request.request_id, request.generation});
    }
    std::deque<State::Done> ready;
    { std::lock_guard lock(state_->mutex); ready.swap(state_->results); }
    for (auto& item : ready) {
        if (!sending || item.source != local_ || !item.source->Active() || item.response.generation != local_generation_) {
            item.response.ok = false; item.response.bytes.clear();
        }
        (void)session.send_clipboard_file_data(item.response);
    }
}
}
