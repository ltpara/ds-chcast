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
// The capture frame rate follows the target bitrate. Under CBR the rate is the
// budget, so cutting frames saves no bandwidth; what it does buy is bits per
// pixel, and at 500 kbps a 1080p frame needs 10 fps to look anything like the
// desktop. The ladder is a pure function of target/ceiling, so it cannot get
// stuck below the ceiling once the path recovers.
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
    // Capture frame rate that goes with the new target. Carried on every
    // decision, not only a change: a caller that missed an earlier change
    // still gets the rate that matches the current target.
    uint16_t framerate = 0;
    // Short stable token for logs and statistics: "loss", "rtt", "queue",
    // "recover". Empty when nothing changed.
    const char* reason = "";
};

// The lowest video rate worth sending for a configured budget: a tenth of it,
// never below 500 kbps and never above the budget itself.
uint32_t video_floor(uint32_t configured_bps);

// What the video encoder may spend when a constant-rate companion stream (the
// host's system audio) already reserves part of the operator's budget. The
// reserve is capped at half the budget so a large audio rate cannot squeeze
// video out entirely, and the floor still applies.
uint32_t video_budget(uint32_t configured_bps, uint32_t reserved_bps);

class BitrateController {
public:
    // `ceiling_bps` is the operator's configured bitrate (or the encoder's
    // auto choice) minus whatever the other streams already reserve; the
    // target never exceeds it. `floor_bps` is the lowest video rate worth
    // sending. `framerate` is the configured capture rate, the top of the
    // ladder. Floor and ceiling are ordered defensively, so an inverted pair
    // cannot open a range.
    BitrateController(uint32_t ceiling_bps, uint32_t floor_bps, uint16_t framerate);

    // Feeds one transport sample. `elapsed_ms` is the time since the previous
    // call and drives cooldowns and the recovery window. Returns the target
    // and, when it moved, why.
    BitrateDecision update(const BitrateSample& sample, uint32_t elapsed_ms);

    uint32_t target_bps() const noexcept { return target_bps_; }
    uint32_t ceiling_bps() const noexcept { return ceiling_bps_; }
    uint32_t floor_bps() const noexcept { return floor_bps_; }
    uint16_t framerate() const noexcept { return framerate_; }

    // The ladder: the capture rate for `target_bps` out of `ceiling_bps`.
    // Public because the host recomputes it after a ceiling change (audio
    // toggled) without waiting for a transport sample.
    static uint16_t framerate_for(uint32_t target_bps, uint32_t ceiling_bps, uint16_t ceiling_fps);

private:
    BitrateDecision lower(uint32_t percent, const char* reason);
    BitrateDecision settle();

    uint32_t ceiling_bps_;
    uint32_t floor_bps_;
    uint32_t target_bps_;
    uint16_t ceiling_fps_;
    uint16_t framerate_;
    uint32_t down_cooldown_ms_ = 0;
    uint32_t up_cooldown_ms_ = 0;
    uint32_t healthy_ms_ = 0;
    uint64_t previous_lost_packets_ = 0;
    uint64_t previous_sent_packets_ = 0;
    bool have_previous_ = false;
};

}  // namespace chast
