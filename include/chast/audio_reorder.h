#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "chast/protocol.h"

namespace chast {

struct AudioPacket {
    AudioHeader header;
    std::vector<uint8_t> payload;
    uint64_t received_steady_us = 0;
    uint64_t reordered_steady_us = 0;
};

class AudioPacketReorderBuffer {
public:
    using Clock = std::chrono::steady_clock;

    explicit AudioPacketReorderBuffer(
        std::chrono::milliseconds wait = std::chrono::milliseconds(20),
        size_t max_packets = 8)
        : wait_(wait), max_packets_(max_packets > 0 ? max_packets : 1) {}

    void reset(uint64_t first_sequence = 0) {
        pending_.clear();
        expected_sequence_ = first_sequence;
    }

    bool push(AudioPacket packet, Clock::time_point received_at = Clock::now()) {
        if (packet.header.seq < expected_sequence_) {
            return false;
        }
        return pending_
            .emplace(packet.header.seq,
                     Entry{std::move(packet), received_at})
            .second;
    }

    bool pop_ready(Clock::time_point now, AudioPacket& packet,
                   uint64_t& missing_sequences, bool force = false) {
        missing_sequences = 0;
        if (pending_.empty()) {
            return false;
        }

        auto next = pending_.begin();
        if (!force && next->first != expected_sequence_ &&
            pending_.size() < max_packets_ && now - next->second.received_at < wait_) {
            return false;
        }

        missing_sequences = next->first - expected_sequence_;
        expected_sequence_ = next->first + 1;
        packet = std::move(next->second.packet);
        pending_.erase(next);
        return true;
    }

    size_t pending_packets() const { return pending_.size(); }
    uint64_t expected_sequence() const { return expected_sequence_; }
    bool has_later_pending(uint64_t sequence) const {
        return pending_.upper_bound(sequence) != pending_.end();
    }

private:
    struct Entry {
        AudioPacket packet;
        Clock::time_point received_at;
    };

    std::chrono::milliseconds wait_;
    size_t max_packets_;
    uint64_t expected_sequence_ = 0;
    std::map<uint64_t, Entry> pending_;
};

}  // namespace chast
