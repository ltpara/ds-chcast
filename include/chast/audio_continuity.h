#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chast {

struct AudioContinuityBudget {
    std::optional<uint64_t> max_starved_ms;
    std::optional<uint64_t> max_device_gap_ms;
    std::optional<uint64_t> max_dropped_blocks;
    std::optional<double> max_abs_av_skew_ms;

    bool enabled() const noexcept {
        return max_starved_ms.has_value() || max_device_gap_ms.has_value() ||
               max_dropped_blocks.has_value() || max_abs_av_skew_ms.has_value();
    }
};

struct AudioContinuityMetrics {
    uint64_t starved_ms = 0;
    uint64_t device_max_gap_ms = 0;
    uint64_t dropped_blocks = 0;
    double av_skew_ms = 0.0;
    bool av_skew_available = false;
};

inline std::vector<std::string> audio_continuity_violations(
    const AudioContinuityBudget& budget, const AudioContinuityMetrics& metrics) {
    std::vector<std::string> violations;
    if (budget.max_starved_ms && metrics.starved_ms > *budget.max_starved_ms) {
        violations.push_back("audio starvation exceeded its budget");
    }
    if (budget.max_device_gap_ms &&
        metrics.device_max_gap_ms > *budget.max_device_gap_ms) {
        violations.push_back("maximum audio device gap exceeded its budget");
    }
    if (budget.max_dropped_blocks &&
        metrics.dropped_blocks > *budget.max_dropped_blocks) {
        violations.push_back("audio playback drops exceeded their budget");
    }
    if (budget.max_abs_av_skew_ms) {
        if (!metrics.av_skew_available || !std::isfinite(metrics.av_skew_ms)) {
            violations.push_back("A/V skew was unavailable for its budget");
        } else if (std::abs(metrics.av_skew_ms) > *budget.max_abs_av_skew_ms) {
            violations.push_back("absolute A/V skew exceeded its budget");
        }
    }
    return violations;
}

}  // namespace chast
