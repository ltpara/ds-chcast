#include "host/shared_loopback.h"
#include <chrono>
#include <deque>

namespace chast {
namespace {

// A stalled reader must not replay half a second of old sound when it
// resumes. The packet cap bounds bytes; this bounds age, which is what the
// far side actually experiences as latency.
constexpr uint64_t kMaxQueuedAudioAgeMs = 500;

uint64_t steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

uint64_t drop_stale(std::deque<EncodedAudio>& packets, uint64_t now) {
    uint64_t dropped = 0;
    while (!packets.empty() && now > packets.front().capture_ms &&
           now - packets.front().capture_ms > kMaxQueuedAudioAgeMs) {
        packets.pop_front();
        ++dropped;
    }
    return dropped;
}

struct AudioSubscription {
    std::deque<EncodedAudio> packets;
    uint64_t dropped = 0;
    uint64_t stale_dropped = 0;
    bool active = false;
};
}
struct SharedLoopback::Source {
    std::mutex mutex;
    std::unique_ptr<ILoopbackCapture> capture;
    std::vector<std::weak_ptr<AudioSubscription>> subscribers;
    bool started = false;
    ~Source() { if (started) capture->stop(); }
};

class SharedLoopback::Reader final : public ILoopbackCapture {
public:
    explicit Reader(std::shared_ptr<Source> source) : source_(std::move(source)) {
        std::lock_guard<std::mutex> lock(source_->mutex);
        source_->subscribers.push_back(sub_);
    }
    void start() override {
        std::lock_guard<std::mutex> lock(source_->mutex);
        sub_->active = true;
        if (!source_->started) {
            source_->capture->start();
            source_->started = true;
        }
    }
    void stop() override {
        std::lock_guard<std::mutex> lock(source_->mutex);
        sub_->active = false;
        sub_->packets.clear();
    }
    bool take_packet(EncodedAudio& packet) override {
        std::lock_guard<std::mutex> lock(source_->mutex);
        if (!sub_->active) return false;
        if (sub_->packets.empty()) {
            EncodedAudio next;
            if (source_->capture->take_packet(next)) {
                const uint64_t now = steady_now_ms();
                for (auto it = source_->subscribers.begin(); it != source_->subscribers.end();) {
                    auto sub = it->lock();
                    if (!sub) { it = source_->subscribers.erase(it); continue; }
                    ++it;
                    if (!sub->active) continue;
                    sub->stale_dropped += drop_stale(sub->packets, now);
                    if (sub->packets.size() >= 25) {
                        sub->packets.pop_front();
                        ++sub->dropped;
                    }
                    sub->packets.push_back(next);
                }
            }
        }
        if (sub_->packets.empty()) return false;
        sub_->stale_dropped += drop_stale(sub_->packets, steady_now_ms());
        if (sub_->packets.empty()) return false;
        packet = std::move(sub_->packets.front());
        sub_->packets.pop_front();
        return true;
    }
    bool failed() const override {
        std::lock_guard<std::mutex> lock(source_->mutex);
        return source_->capture->failed();
    }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(source_->mutex);
        return source_->capture->error();
    }
    AudioInfo info() const override {
        std::lock_guard<std::mutex> lock(source_->mutex);
        return source_->capture->info();
    }
    AudioCaptureStats stats() const override {
        std::lock_guard<std::mutex> lock(source_->mutex);
        AudioCaptureStats stats = source_->capture->stats();
        stats.dropped += sub_->dropped;
        stats.stale_dropped += sub_->stale_dropped;
        return stats;
    }
private:
    std::shared_ptr<Source> source_;
    std::shared_ptr<AudioSubscription> sub_ = std::make_shared<AudioSubscription>();
};

std::unique_ptr<ILoopbackCapture> SharedLoopback::subscribe(Factory factory, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto source = source_.lock();
    if (!source) {
        auto capture = factory(error);
        if (!capture) return nullptr;
        source = std::make_shared<Source>();
        source->capture = std::move(capture);
        source_ = source;
    }
    return std::make_unique<Reader>(std::move(source));
}
} // namespace chast
