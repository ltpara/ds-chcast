#pragma once

// Pure-logic adaptive bitrate controller for the host encoder.
//
// The controller watches the transport, not the desktop: screen content is
// variable bitrate, so a send rate below the encoder target says nothing about
// available bandwidth. What does matter is RTT growing past its own minimum
// (bufferbloat), actual packet loss, and the host's send queue filling the
// congestion window. Those lower the target; a sustained clean period raises
// it again, never past the configured ceiling.
//
// No Win32, GPU or network dependency: tests drive it with synthetic samples.

#include <cstdint>

namespace chast {

struct BitrateSample {
    // RTT readings in microseconds, 0 when the transport has no sample yet.
    uint32_t rtt_us = 0;
    uint32_t min_rtt_us = 0;
    // Application bytes waiting for send completion, and the transport's
    // current congestion window. With no congestion window (0) queue pressure
    // is not assessed.
    uint64_t queued_bytes = 0;
    uint64_t congestion_window = 0;
    // Cumulative transport counters. The controller keeps the previous sample
    // and derives per-interval loss itself.
    uint64_t sent_bytes = 0;
    uint64_t lost_packets = 0;
    uint64_t sent_packets = 0;
};

struct BitrateDecision {
    bool changed = false;
    uint32_t target_bps = 0;
    // Short stable token for logs and statistics: "loss", "rtt", "queue",
    // "recover". Empty when nothing changed.
    const char* reason = "";
};

class BitrateController {
public:
    // `ceiling_bps` is the operator's configured bitrate (or the encoder's
    // auto choice); the target never exceeds it. Floor and ceiling are
    // ordered defensively, so an inverted pair cannot open a range.
    BitrateController(uint32_t ceiling_bps, uint32_t floor_bps);

    // Feeds one transport sample. `elapsed_ms` is the time since the previous
    // call and drives cooldowns and the recovery window. Returns the target
    // and, when it moved, why.
    BitrateDecision update(const BitrateSample& sample, uint32_t elapsed_ms);

    uint32_t target_bps() const noexcept { return target_bps_; }
    uint32_t ceiling_bps() const noexcept { return ceiling_bps_; }
    uint32_t floor_bps() const noexcept { return floor_bps_; }

private:
    BitrateDecision lower(uint32_t percent, const char* reason);

    uint32_t ceiling_bps_;
    uint32_t floor_bps_;
    uint32_t target_bps_;
    uint32_t down_cooldown_ms_ = 0;
    uint32_t up_cooldown_ms_ = 0;
    uint32_t healthy_ms_ = 0;
    uint64_t previous_lost_packets_ = 0;
    uint64_t previous_sent_packets_ = 0;
    bool have_previous_ = false;
};

}  // namespace chast
