// Deterministic checks for the host-wide system-audio fan-out: every reader
// gets its own bounded queue, a short host-loop stall must not discard sound,
// a sustained stall drops only audio older than 500 ms, and packet overflow is
// reported separately from stale-age eviction.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "host/shared_loopback.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

class SyntheticCapture final : public chast::ILoopbackCapture {
public:
    explicit SyntheticCapture(std::vector<uint64_t> capture_ms)
        : capture_ms_(std::move(capture_ms)) {}

    void start() override { started_ = true; }
    void stop() override { started_ = false; }
    bool take_packet(chast::EncodedAudio& packet) override {
        if (!started_ || next_ >= capture_ms_.size()) return false;
        packet.capture_ms = capture_ms_[next_];
        packet.data = {static_cast<uint8_t>(next_ & 0xFF), 0x5a};
        ++next_;
        ++stats_.packets;
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
        info.frame_samples = chast::kOpusFrameSamples;
        return info;
    }
    chast::AudioCaptureStats stats() const override { return stats_; }

private:
    std::vector<uint64_t> capture_ms_;
    size_t next_ = 0;
    bool started_ = false;
    chast::AudioCaptureStats stats_;
};

void test_fanout_and_short_stall() {
    const uint64_t base = now_ms() - 200;
    chast::SharedLoopback loopback;
    std::string error;
    auto a = loopback.subscribe(
        [&](std::string*) { return std::make_unique<SyntheticCapture>(
            std::vector<uint64_t>{base, base + 20, base + 40}); },
        &error);
    auto b = loopback.subscribe(
        [&](std::string*) { return std::make_unique<SyntheticCapture>(
            std::vector<uint64_t>{base, base + 20, base + 40}); },
        &error);
    check(a != nullptr && b != nullptr, "two readers subscribe to one system-audio source");
    if (!a || !b) return;
    a->start();
    b->start();
    chast::EncodedAudio first;
    check(a->take_packet(first) && first.capture_ms == base,
          "the first reader keeps a packet produced before a short stall");
    check(b->take_packet(first) && first.capture_ms == base,
          "the second reader has an independent copy of the same packet");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    uint64_t count = 1;
    while (a->take_packet(first)) ++count;
    check(count == 3, "the short stall drains every produced packet");
    const chast::AudioCaptureStats stats = a->stats();
    check(stats.dropped == 0 && stats.stale_dropped == 0,
          "a stall shorter than the age budget does not count as loss");
}

void test_overflow() {
    const uint64_t base = now_ms() - 450;
    std::vector<uint64_t> packets;
    for (uint64_t i = 0; i < 26; ++i) packets.push_back(base + i);
    chast::SharedLoopback loopback;
    std::string error;
    auto slow = loopback.subscribe(
        [&](std::string*) { return std::make_unique<SyntheticCapture>(packets); },
        &error);
    auto driver = loopback.subscribe(
        [&](std::string*) { return std::make_unique<SyntheticCapture>(packets); },
        &error);
    check(slow != nullptr && driver != nullptr, "a slow reader and driver subscribe");
    if (!slow || !driver) return;
    slow->start();
    driver->start();
    chast::EncodedAudio packet;
    for (size_t i = 0; i < packets.size(); ++i) {
        if (!driver->take_packet(packet)) {
            check(false, "the driver drains the synthetic source");
            return;
        }
    }
    const chast::AudioCaptureStats stats = slow->stats();
    check(stats.dropped == 1 && stats.stale_dropped == 0,
          "queue overflow is reported separately from age eviction");
}

void test_stale_age() {
    const uint64_t base = now_ms() - 800;
    chast::SharedLoopback loopback;
    std::string error;
    auto reader = loopback.subscribe(
        [&](std::string*) { return std::make_unique<SyntheticCapture>(
            std::vector<uint64_t>{base, base + 10, base + 20}); },
        &error);
    check(reader != nullptr, "the stale-age reader subscribes");
    if (!reader) return;
    reader->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    chast::EncodedAudio packet;
    check(!reader->take_packet(packet), "packets older than 500 ms are not replayed");
    const chast::AudioCaptureStats stale = reader->stats();
    check(stale.stale_dropped > 0 && stale.dropped == 0,
          "age eviction is reported separately from queue overflow");
}
} // namespace

int main() {
    test_fanout_and_short_stall();
    test_overflow();
    test_stale_age();
    if (failures) {
        std::printf("%d shared-audio check(s) failed\n", failures);
        return 1;
    }
    std::printf("all shared-audio checks passed\n");
    return 0;
}
