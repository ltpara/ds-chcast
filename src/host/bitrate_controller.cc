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

}  // namespace

BitrateController::BitrateController(uint32_t ceiling_bps, uint32_t floor_bps)
    : ceiling_bps_(std::max<uint32_t>(ceiling_bps, 1)),
      floor_bps_(std::min(std::max<uint32_t>(floor_bps, 1), std::max<uint32_t>(ceiling_bps, 1))),
      target_bps_(std::max<uint32_t>(ceiling_bps, 1)) {}

BitrateDecision BitrateController::lower(uint32_t percent, const char* reason) {
    BitrateDecision decision;
    const uint32_t next =
        std::max(floor_bps_, static_cast<uint32_t>(target_bps_ * percent / 100u));
    if (next >= target_bps_) {
        decision.target_bps = target_bps_;
        return decision;
    }
    target_bps_ = next;
    down_cooldown_ms_ = kDownCooldownMs;
    up_cooldown_ms_ = kUpCooldownMs;
    healthy_ms_ = 0;
    decision.changed = true;
    decision.target_bps = target_bps_;
    decision.reason = reason;
    return decision;
}

BitrateDecision BitrateController::update(const BitrateSample& sample, uint32_t elapsed_ms) {
    BitrateDecision decision;
    decision.target_bps = target_bps_;

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

    const bool loss_valid = sent_delta >= kMinPacketsForLoss;
    const double loss_ratio =
        loss_valid ? static_cast<double>(lost_delta) / static_cast<double>(sent_delta) : 0.0;

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
        if (down_cooldown_ms_ == 0) {
            if (loss_valid && loss_ratio > kModerateLossRatio) {
                return lower(severe ? 70 : 85, "loss");
            }
            if (rtt_pressure) {
                return lower(severe ? 70 : 85, "rtt");
            }
            return lower(severe ? 70 : 85, "queue");
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
    decision.changed = true;
    decision.target_bps = target_bps_;
    decision.reason = "recover";
    return decision;
}

}  // namespace chast
