#include "host/shared_loopback.h"
#include <deque>

namespace chast {
namespace {
struct AudioSubscription {
    std::deque<EncodedAudio> packets;
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
                for (auto it = source_->subscribers.begin(); it != source_->subscribers.end();) {
                    auto sub = it->lock();
                    if (!sub) { it = source_->subscribers.erase(it); continue; }
                    ++it;
                    if (!sub->active) continue;
                    if (sub->packets.size() >= 25) sub->packets.pop_front();
                    sub->packets.push_back(next);
                }
            }
        }
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
        return source_->capture->stats();
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
