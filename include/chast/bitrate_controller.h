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
// A percent cut every cooldown is the right response to a slow drift, but not
// to a capacity step: while the target walks down 15-30% at a time the
// encoder keeps producing above the bottleneck, and a reliable ordered stream
// can only queue the excess -- that is seconds of frame age. When delivery
// collapses under 70% of the target while the send queue has filled most of the
// congestion window, the delivered rate (the wire rate discounted by the
// interval's loss share) is the budget the step left, so the controller lands
// just under it in one decision. The delivered rate never sets a target the
// percent cut would not also allow. A path that still carries most of the
// target -- an RTT rise without a capacity change -- keeps the percent cut, and
// so does a low wire rate with an idle queue, which is content, not capacity.
//
// The capture frame rate follows the target bitrate. Under CBR the rate is the
// budget, so cutting frames saves no bandwidth; what it does buy is bits per
// pixel, and at 500 kbps a 1080p frame needs 10 fps to look anything like the
// desktop. The ladder is a pure function of target/ceiling, so it cannot get
// stuck below the ceiling once the path recovers.
//
// Resolution follows the same ratio on a coarser, hysteretic ladder: full size
// above half the ceiling, 75% from 30% up, 50% below that, and the way back up
// needs more headroom than the way down. Resolution changes are far more
// expensive than frame-rate changes -- the encoder is recreated and the
// viewer must decode a new size -- so a boundary cannot toggle on jitter.
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
    // and derives per-interval loss and wire rate itself. `sent_bytes` is the
    // transport's total send volume (every stream, packet headers included),
    // so its delta is what the path actually carried; 0 when the transport
    // cannot report it.
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
    // Encoded picture size as a percentage of the captured frame (M6.1's
    // resolution ladder): 100, 75 or 50. Carried like `framerate`, so a caller
    // that applies a decision late still lands on the right size.
    uint16_t resolution_percent = 100;
    // Short stable token for logs and statistics: "loss", "rtt", "queue",
    // "recover", "framerate", "resolution". Empty when nothing changed.
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
    // ladder. `resolution_percent` is the rung the caller is already on (one
    // of 50/75/100), so rebuilding the controller does not silently reset a
    // downscaled stream. Floor and ceiling are ordered defensively, so an
    // inverted pair cannot open a range.
    BitrateController(uint32_t ceiling_bps, uint32_t floor_bps, uint16_t framerate,
                      uint16_t resolution_percent = 100);

    // Feeds one transport sample. `elapsed_ms` is the time since the previous
    // call and drives cooldowns and the recovery window. Returns the target
    // and, when it moved, why.
    BitrateDecision update(const BitrateSample& sample, uint32_t elapsed_ms);

    uint32_t target_bps() const noexcept { return target_bps_; }
    uint32_t ceiling_bps() const noexcept { return ceiling_bps_; }
    uint32_t floor_bps() const noexcept { return floor_bps_; }
    uint16_t framerate() const noexcept { return framerate_; }
    uint16_t resolution_percent() const noexcept { return resolution_percent_; }

    // Applies a viewer quality preset: a new ceiling with the target restarted
    // at it, because the operator just asked for that budget and the ladder
    // walks it back down from transport samples. The current resolution rung
    // is kept so a preference change does not trigger an encoder recreation;
    // the caller's own scale and the next samples settle it. Clamped to the
    // floor, which is far below any quality preset in practice.
    BitrateDecision set_ceiling(uint32_t ceiling_bps);

    // Changes the top of the frame-rate ladder (the viewer's cap versus the
    // host's configured rate). The frame rate is recomputed from the current
    // target/ceiling ratio; the target itself does not move.
    BitrateDecision set_top_framerate(uint16_t framerate);

    // The ladder: the capture rate for `target_bps` out of `ceiling_bps`.
    // Public because the host recomputes it after a ceiling change (audio
    // toggled) without waiting for a transport sample.
    static uint16_t framerate_for(uint32_t target_bps, uint32_t ceiling_bps, uint16_t ceiling_fps);

    // The resolution ladder, including its hysteresis: `current_percent` is the
    // rung the stream is on now, and recovering needs a higher ratio than
    // dropping did.
    static uint16_t resolution_for(uint32_t target_bps, uint32_t ceiling_bps,
                                   uint16_t current_percent);

private:
    // `delivered_bps` is the interval's wire rate discounted by its loss
    // share, 0 when there was no usable byte counter; a positive value can
    // only pull the target further down, never up.
    BitrateDecision lower(uint32_t percent, const char* reason, uint32_t delivered_bps);
    BitrateDecision settle();

    uint32_t ceiling_bps_;
    uint32_t floor_bps_;
    uint32_t target_bps_;
    uint16_t ceiling_fps_;
    uint16_t framerate_;
    uint16_t resolution_percent_;
    uint32_t down_cooldown_ms_ = 0;
    uint32_t up_cooldown_ms_ = 0;
    uint32_t healthy_ms_ = 0;
    uint64_t previous_lost_packets_ = 0;
    uint64_t previous_sent_packets_ = 0;
    uint64_t previous_sent_bytes_ = 0;
    bool have_previous_ = false;
    bool have_previous_bytes_ = false;
};

}  // namespace chast
