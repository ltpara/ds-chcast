#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "chast/audio.h"
#include "chast/protocol.h"

namespace chast {

// Owns only the forwarding worker. Its caller owns capture and transport, and
// must stop this worker before resetting either. No session/UI statistics are
// written by the worker; the session thread merges snapshots.
class AudioForwarder {
public:
    struct Stats {
        uint64_t packets = 0, bytes = 0, dropped = 0, errors = 0;
        uint64_t max_submit_gap_ms = 0, max_capture_age_ms = 0;
        std::vector<std::string> failures;
    };
    using Send = std::function<Error(AudioHeader, const uint8_t*, size_t)>;

    ~AudioForwarder() { stop(); }

    void start(ILoopbackCapture& capture, AudioInfo info, uint64_t base_ms, Send send) {
        if (worker_.joinable()) return;
        worker_ = std::jthread([this, &capture, info, base_ms,
                                send = std::move(send)](std::stop_token stop) {
            uint64_t last_submit_ms = 0;
            while (!stop.stop_requested()) {
                EncodedAudio packet;
                if (!capture.take_packet(packet)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                AudioHeader header;
                header.capture_ms = packet.capture_ms & 0xFFFFFFFFFFFFull;
                header.pts_ms = header.capture_ms >= base_ms ? header.capture_ms - base_ms : 0;
                header.sample_rate = info.sample_rate;
                header.frame_samples = info.frame_samples;
                header.channels = info.channels;
                header.codec = info.codec;
                const auto submitted_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const Error sent = send(header, packet.data.data(), packet.data.size());
                if (sent.status() == Status::PermissionDenied) continue;
                std::lock_guard lock(mutex_);
                if (last_submit_ms && submitted_ms >= last_submit_ms)
                    stats_.max_submit_gap_ms = (std::max)(stats_.max_submit_gap_ms,
                                                         submitted_ms - last_submit_ms);
                last_submit_ms = submitted_ms;
                if (submitted_ms >= header.capture_ms)
                    stats_.max_capture_age_ms = (std::max)(stats_.max_capture_age_ms,
                                                          submitted_ms - header.capture_ms);
                if (sent.ok()) {
                    ++stats_.packets;
                    stats_.bytes += packet.data.size();
                } else {
                    if (sent.status() == Status::WaitTimeout) ++stats_.dropped;
                    else ++stats_.errors;
                    if (stats_.failures.size() < 3) stats_.failures.push_back(sent.to_string());
                }
            }
        });
    }

    void stop() {
        if (!worker_.joinable()) return;
        worker_.request_stop();
        worker_.join();
    }

    Stats stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

private:
    mutable std::mutex mutex_;
    Stats stats_;
    std::jthread worker_;
};

}  // namespace chast
