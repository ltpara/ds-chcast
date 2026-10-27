#include "chast/bitrate_controller.h"

#include <algorithm>

namespace chast {

namespace {

// A sample with fewer packets than this cannot distinguish a few losses from
// a noisy counter, so loss is only assessed on a meaningful denominator.
constexpr uint64_t kMinPacketsForLoss = 8;
// RTT must clear both a relative and an absolute margin before it counts as
// bufferbloat; a local link has a sub-millisecond minimum, where a purely
// relative threshold would fire on scheduling noise.
constexpr uint32_t kRttAbsoluteMarginUs = 25000;
constexpr uint32_t kDownCooldownMs = 1500;
constexpr uint32_t kUpCooldownMs = 3000;
constexpr uint32_t kHealthyWindowMs = 3000;

constexpr double kSevereLossRatio = 0.10;
constexpr double kModerateLossRatio = 0.03;
constexpr double kHealthyLossRatio = 0.01;
constexpr double kQueueSevereRatio = 1.00;
constexpr double kQueueModerateRatio = 0.65;
constexpr double kQueueHealthyRatio = 0.50;

// A capacity step down is handled in one decision: the target lands well under
// the measured wire rate. The measured rate over the first interval after a
// step still includes the old target's overshoot (retransmits and the queue
// draining), so the margin has to be wide enough that even an inflated sample
// leaves the target below the new capacity -- landing just above it keeps the
// queue growing for another interval. It also drains the frames the old
// target already queued.
constexpr uint32_t kDeliveryHeadroomPercent = 65;
// A rate needs a window and enough bytes to be meaningful; a short or tiny
// interval is dominated by scheduling noise and must not move the target.
constexpr uint32_t kMinRateWindowMs = 400;
constexpr uint64_t kMinRateBytes = 16384;
// A cooldown exists to keep one noisy sample from moving the target twice, but
// delivery collapsing under this share of the target while the send queue is
// filling the congestion window is not noise -- it is a capacity step. Such a
// sample bypasses the cooldown and sets the landing point. A low wire rate
// with an idle queue is content, not capacity (screen updates are bursty), so
// it keeps the percent cut.
constexpr uint32_t kDeliveryCollapsePercent = 70;

// Frame-rate ladder, as a share of the ceiling. A 1080p screen at 500 kbps
// needs about 10 fps to spend 0.02 bits per pixel; below that the desktop
// turns into colour blocks, so the ladder bottoms out there.
constexpr double kFullFramerateRatio = 0.70;
constexpr double kTwoThirdsRatio = 0.45;
constexpr double kHalfRatio = 0.25;
constexpr uint16_t kMinimumFramerate = 10;

// Resolution ladder, as a share of the ceiling. Every rung costs an encoder
// recreation and a keyframe, so there are only three and the up edges sit
// above the down edges (0.50 down vs 0.55 up for full, 0.30 vs 0.34 for three
// quarters) to keep a target hovering on an edge from toggling the size.
constexpr double kFullResolutionRatio = 0.50;
constexpr double kThreeQuarterResolutionRatio = 0.30;
constexpr double kRestoreFullResolutionRatio = 0.55;
constexpr double kRestoreThreeQuarterResolutionRatio = 0.34;
constexpr uint16_t kMinimumResolutionPercent = 50;
constexpr uint16_t kThreeQuarterResolutionPercent = 75;

}  // namespace

uint32_t video_floor(uint32_t configured_bps) {
    return std::min(configured_bps, std::max<uint32_t>(500000u, configured_bps / 10u));
}

uint32_t video_budget(uint32_t configured_bps, uint32_t reserved_bps) {
    const uint32_t reserve = std::min(reserved_bps, configured_bps / 2u);
    return std::max(video_floor(configured_bps), configured_bps - reserve);
}

BitrateController::BitrateController(uint32_t ceiling_bps, uint32_t floor_bps, uint16_t framerate,
                                     uint16_t resolution_percent)
    : ceiling_bps_(std::max<uint32_t>(ceiling_bps, 1)),
      floor_bps_(std::min(std::max<uint32_t>(floor_bps, 1), std::max<uint32_t>(ceiling_bps, 1))),
      target_bps_(std::max<uint32_t>(ceiling_bps, 1)),
      ceiling_fps_(framerate == 0 ? 30 : framerate),
      framerate_(ceiling_fps_),
      resolution_percent_(resolution_percent >= 100
                              ? 100
                              : (resolution_percent >= kThreeQuarterResolutionPercent
                                     ? kThreeQuarterResolutionPercent
                                     : kMinimumResolutionPercent)) {}

uint16_t BitrateController::framerate_for(uint32_t target_bps, uint32_t ceiling_bps,
                                          uint16_t ceiling_fps) {
    const uint32_t ceiling = std::max<uint32_t>(ceiling_bps, 1);
    const double ratio = static_cast<double>(target_bps) / static_cast<double>(ceiling);
    const uint16_t top = ceiling_fps == 0 ? 30 : ceiling_fps;
    uint32_t fps = top;
    if (ratio < kHalfRatio) {
        fps = top / 3;
    } else if (ratio < kTwoThirdsRatio) {
        fps = top / 2;
    } else if (ratio < kFullFramerateRatio) {
        fps = top * 2 / 3;
    }
    fps = std::clamp<uint32_t>(fps, std::min<uint32_t>(kMinimumFramerate, top), top);
    return static_cast<uint16_t>(fps);
}

uint16_t BitrateController::resolution_for(uint32_t target_bps, uint32_t ceiling_bps,
                                           uint16_t current_percent) {
    const uint32_t ceiling = std::max<uint32_t>(ceiling_bps, 1);
    // Left as a fraction so the constants above read as the ratios they are.
    // The comparisons are per-rung: recovering or dropping goes at most two
    // rungs in one decision, and only when the ratio skipped both edges (a
    // large ceiling change), so the viewer never sees two resizes in a row.
    const double ratio = static_cast<double>(target_bps) / static_cast<double>(ceiling);
    const auto full = static_cast<uint16_t>(100);
    if (current_percent <= kMinimumResolutionPercent) {
        if (ratio >= kRestoreFullResolutionRatio) {
            return full;
        }
        return ratio >= kRestoreThreeQuarterResolutionRatio ? kThreeQuarterResolutionPercent
                                                            : kMinimumResolutionPercent;
    }
    if (current_percent <= kThreeQuarterResolutionPercent) {
        if (ratio >= kRestoreFullResolutionRatio) {
            return full;
        }
        return ratio < kThreeQuarterResolutionRatio ? kMinimumResolutionPercent
                                                    : kThreeQuarterResolutionPercent;
    }
    if (ratio < kThreeQuarterResolutionRatio) {
        return kMinimumResolutionPercent;
    }
    return ratio < kFullResolutionRatio ? kThreeQuarterResolutionPercent : full;
}

// Fills in the fields a decision carries when the target did not move. The
// frame rate and resolution always follow the target, so they are recomputed
// every call and a ceiling change (audio toggled) shows up without waiting on
// the transport.
BitrateDecision BitrateController::set_ceiling(uint32_t ceiling_bps) {
    const uint32_t next = std::max<uint32_t>(ceiling_bps, floor_bps_);
    BitrateDecision decision;
    decision.target_bps = target_bps_;
    decision.framerate = framerate_;
    decision.resolution_percent = resolution_percent_;
    if (next == ceiling_bps_ && target_bps_ == next) {
        return decision;
    }
    ceiling_bps_ = next;
    target_bps_ = next;
    framerate_ = framerate_for(target_bps_, ceiling_bps_, ceiling_fps_);
    // A preset change is a deliberate budget decision, not congestion, so it
    // clears both cooldowns: the next sample may adjust immediately.
    healthy_ms_ = 0;
    down_cooldown_ms_ = 0;
    up_cooldown_ms_ = 0;
    decision.changed = true;
    decision.target_bps = target_bps_;
    decision.framerate = framerate_;
    decision.reason = "quality";
    return decision;
}

BitrateDecision BitrateController::set_top_framerate(uint16_t framerate) {
    ceiling_fps_ = framerate == 0 ? 30 : framerate;
    const uint16_t next = framerate_for(target_bps_, ceiling_bps_, ceiling_fps_);
    BitrateDecision decision;
    decision.target_bps = target_bps_;
    decision.framerate = next;
    decision.resolution_percent = resolution_percent_;
    if (next != framerate_) {
        framerate_ = next;
        decision.changed = true;
        decision.reason = "framerate";
    }
    return decision;
}

BitrateDecision BitrateController::settle() {
    BitrateDecision decision;
    decision.target_bps = target_bps_;
    decision.framerate = framerate_for(target_bps_, ceiling_bps_, ceiling_fps_);
    decision.resolution_percent = resolution_for(target_bps_, ceiling_bps_, resolution_percent_);
    if (decision.framerate != framerate_) {
        framerate_ = decision.framerate;
        decision.changed = true;
        decision.reason = "framerate";
    }
    if (decision.resolution_percent != resolution_percent_) {
        resolution_percent_ = decision.resolution_percent;
        decision.changed = true;
        if (decision.reason[0] == '\0') {
            decision.reason = "resolution";
        }
    }
    return decision;
}

BitrateDecision BitrateController::lower(uint32_t percent, const char* reason,
                                         uint32_t delivered_bps) {
    uint32_t next =
        std::max(floor_bps_, static_cast<uint32_t>(target_bps_ * percent / 100u));
    // The percent cut is right for a slow drift, but on a capacity step it
    // walks down for seconds while the encoder keeps overshooting the new
    // bottleneck. The caller passes a rate only when delivery collapsed under
    // an overloaded send queue, so the delivered rate says where the step
    // landed: land just under it, with the floor still applying.
    if (delivered_bps > 0) {
        const uint32_t landing = static_cast<uint32_t>(
            std::min<uint64_t>(static_cast<uint64_t>(delivered_bps) * kDeliveryHeadroomPercent / 100u,
                               static_cast<uint64_t>(target_bps_)));
        next = std::min(next, std::max(floor_bps_, landing));
    }
    if (next >= target_bps_) {
        return settle();
    }
    target_bps_ = next;
    down_cooldown_ms_ = kDownCooldownMs;
    up_cooldown_ms_ = kUpCooldownMs;
    healthy_ms_ = 0;
    // settle() brings the frame rate and resolution along; the congestion
    // reason wins over its "framerate"/"resolution" tokens.
    BitrateDecision decision = settle();
    decision.changed = true;
    decision.reason = reason;
    return decision;
}

BitrateDecision BitrateController::update(const BitrateSample& sample, uint32_t elapsed_ms) {
    BitrateDecision decision = settle();

    uint64_t sent_delta = 0;
    uint64_t lost_delta = 0;
    if (have_previous_ && sample.sent_packets >= previous_sent_packets_ &&
        sample.lost_packets >= previous_lost_packets_) {
        sent_delta = sample.sent_packets - previous_sent_packets_;
        lost_delta = sample.lost_packets - previous_lost_packets_;
    }
    previous_lost_packets_ = sample.lost_packets;
    previous_sent_packets_ = sample.sent_packets;
    have_previous_ = true;

    // The wire rate between two samples: transport send bytes over the
    // interval. A reconnect can reset the counter, so a decrease is treated as
    // no sample rather than a negative rate. The counter includes packets the
    // path dropped and their retransmits, so it is discounted by the interval
    // loss ratio below before it is used as the delivered rate.
    uint32_t delivered_bps = 0;
    if (have_previous_bytes_ && sample.sent_bytes >= previous_sent_bytes_) {
        const uint64_t byte_delta = sample.sent_bytes - previous_sent_bytes_;
        if (elapsed_ms >= kMinRateWindowMs && byte_delta >= kMinRateBytes) {
            // bytes * 8 * 1000 / milliseconds is exactly bits per second.
            delivered_bps = static_cast<uint32_t>(
                std::min<uint64_t>(byte_delta * 8000u / elapsed_ms, 0xffffffffull));
        }
    }
    previous_sent_bytes_ = sample.sent_bytes;
    have_previous_bytes_ = true;

    const bool loss_valid = sent_delta >= kMinPacketsForLoss;
    const double loss_ratio =
        loss_valid ? static_cast<double>(lost_delta) / static_cast<double>(sent_delta) : 0.0;
    // Under a capacity step every attempt over the bottleneck is dropped and
    // retransmitted, so the send counter can sit far above what actually
    // arrives -- the loss share is the part the path did not deliver. Without
    // this the fast cut can land above the new capacity and the queue keeps
    // growing for another interval. A spurious-loss counter can overreport
    // for one interval, so the share is clamped before it scales the rate.
    if (delivered_bps > 0 && loss_valid && loss_ratio > 0.0) {
        const double keep = loss_ratio >= 1.0 ? 0.0 : 1.0 - loss_ratio;
        delivered_bps =
            static_cast<uint32_t>(static_cast<double>(delivered_bps) * keep);
    }

    bool rtt_pressure = false;
    if (sample.rtt_us > 0 && sample.min_rtt_us > 0) {
        const uint64_t margin =
            std::max<uint64_t>(static_cast<uint64_t>(sample.min_rtt_us) + kRttAbsoluteMarginUs,
                               static_cast<uint64_t>(sample.min_rtt_us) * 3 / 2);
        rtt_pressure = sample.rtt_us > margin;
    }

    bool queue_pressure = false;
    double queue_ratio = 0.0;
    if (sample.congestion_window > 0) {
        queue_ratio =
            static_cast<double>(sample.queued_bytes) / static_cast<double>(sample.congestion_window);
        queue_pressure = true;
    }

    down_cooldown_ms_ =
        elapsed_ms >= down_cooldown_ms_ ? 0 : down_cooldown_ms_ - elapsed_ms;
    up_cooldown_ms_ = elapsed_ms >= up_cooldown_ms_ ? 0 : up_cooldown_ms_ - elapsed_ms;

    const bool severe = (loss_valid && loss_ratio > kSevereLossRatio) ||
                        (queue_pressure && queue_ratio > kQueueSevereRatio);
    const bool moderate = (loss_valid && loss_ratio > kModerateLossRatio) || rtt_pressure ||
                          (queue_pressure && queue_ratio > kQueueModerateRatio);

    if (severe || moderate) {
        healthy_ms_ = 0;
        // A wire rate below the target is only a capacity signal when the send
        // queue is overloaded: an idle desktop on a healthy path also sends
        // far below the target. With both the collapse and the queue, the rate
        // is what the step left. The first sample after a step still shows the
        // old rate and can only cut by percent, which arms the cooldown; the
        // next one cuts through it, or the queue would hold another full
        // interval of overshoot.
        const bool delivery_step =
            delivered_bps > 0 && queue_pressure && queue_ratio > kQueueModerateRatio &&
            static_cast<uint64_t>(delivered_bps) * 100u <
                static_cast<uint64_t>(target_bps_) * kDeliveryCollapsePercent;
        if (down_cooldown_ms_ == 0 || delivery_step) {
            const uint32_t landing = delivery_step ? delivered_bps : 0;
            if (loss_valid && loss_ratio > kModerateLossRatio) {
                return lower(severe ? 70 : 85, "loss", landing);
            }
            if (rtt_pressure) {
                return lower(severe ? 70 : 85, "rtt", landing);
            }
            return lower(severe ? 70 : 85, "queue", landing);
        }
        return decision;
    }

    const bool healthy = (!loss_valid || loss_ratio < kHealthyLossRatio) && !rtt_pressure &&
                         (!queue_pressure || queue_ratio < kQueueHealthyRatio);
    if (!healthy) {
        healthy_ms_ = 0;
        return decision;
    }

    healthy_ms_ = std::min<uint32_t>(healthy_ms_ + elapsed_ms, kHealthyWindowMs * 2);
    if (up_cooldown_ms_ != 0 || healthy_ms_ < kHealthyWindowMs || target_bps_ >= ceiling_bps_) {
        return decision;
    }

    const uint32_t step = std::max<uint32_t>(100000, ceiling_bps_ / 50);
    const uint32_t next = std::min(ceiling_bps_, target_bps_ + step);
    if (next <= target_bps_) {
        return decision;
    }
    target_bps_ = next;
    healthy_ms_ = 0;
    up_cooldown_ms_ = kUpCooldownMs;
    BitrateDecision moved = settle();
    moved.changed = true;
    moved.reason = "recover";
    return moved;
}

}  // namespace chast
