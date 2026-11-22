#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "host/audio_forwarder.h"

namespace {
using Clock = std::chrono::steady_clock;
uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

class Capture final : public chast::ILoopbackCapture {
public:
    void start() override { next_ms_ = now_ms(); }
    void stop() override {}
    bool take_packet(chast::EncodedAudio& packet) override {
        const auto now = now_ms();
        if (now < next_ms_) return false;
        packet.capture_ms = next_ms_;
        next_ms_ += 10;
        packet.data = {1, 2, 3};
        return true;
    }
    bool failed() const override { return false; }
    std::string error() const override { return {}; }
    chast::AudioInfo info() const override {
        chast::AudioInfo info;
        info.enabled = true;
        info.codec = chast::AudioCodec::Opus;
        info.sample_rate = 48000;
        info.channels = 2;
        info.frame_samples = 480;
        return info;
    }
    chast::AudioCaptureStats stats() const override { return {}; }
private:
    uint64_t next_ms_ = 0;
};
}

int main() {
    Capture capture;
    capture.start();
    chast::AudioForwarder sender;
    std::atomic<unsigned> attempts{0};
    std::atomic<bool> revoked{false};
    std::atomic<bool> invalid_header{false};
    sender.start(capture, capture.info(), 0,
        [&](chast::AudioHeader header, const uint8_t*, size_t size) {
            if (header.sample_rate != 48000 || header.channels != 2 ||
                header.frame_samples != 480 || size != 3) invalid_header = true;
            const unsigned attempt = ++attempts;
            if (revoked) return chast::Error(chast::Status::PermissionDenied, "revoked");
            if (attempt == 1) return chast::Error(chast::Status::WaitTimeout, "full");
            if (attempt == 2) return chast::Error(chast::Status::Unavailable, "offline");
            return chast::Error::success();
        });
    // No main-loop drain: video/control work is blocked for 400 ms.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto flowing = sender.stats();
    revoked = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sender.stop();
    const auto stopped = sender.stats();
    const auto final_attempts = attempts.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool ok = !invalid_header && flowing.packets >= 10 && flowing.dropped == 1 &&
        flowing.errors == 1 && stopped.dropped == 1 && stopped.errors == 1 &&
        stopped.packets <= flowing.packets + 1 && attempts == final_attempts &&
        stopped.bytes == stopped.packets * 3 && stopped.failures.size() == 2;
    std::printf("audio forwarder: packets=%llu drops=%llu errors=%llu stopped=%s\n",
        static_cast<unsigned long long>(stopped.packets),
        static_cast<unsigned long long>(stopped.dropped),
        static_cast<unsigned long long>(stopped.errors), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
