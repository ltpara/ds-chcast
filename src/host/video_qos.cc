#include "host/video_qos.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace chast {
namespace {
constexpr uint32_t kInitFps = 15;
constexpr uint32_t kMinAutoFps = 5;
constexpr uint32_t kDelayThresholdMs = 150;
constexpr uint8_t kRestoreGuardSamples = 5;
constexpr uint8_t kStartupEnded = 255;

float clamp_ratio(float value, float low, float high) {
    if (low > high) std::swap(low, high);
    return std::clamp(value, low, high);
}
} // namespace

VideoQos::VideoQos(uint32_t max_fps, float default_ratio, bool abr, bool adaptive_fps)
    : max_fps_(std::clamp(max_fps ? max_fps : 30u, 1u, 120u)), fps_(max_fps_),
      default_ratio_(std::clamp(default_ratio, 0.1f, 40.f)), ratio_(default_ratio_),
      abr_(abr), adaptive_fps_(adaptive_fps) {}

float VideoQos::quality_ratio(Quality quality) {
    switch (quality) {
        case Quality::High: return 1.5f;
        case Quality::Balanced: return 0.67f;
        case Quality::Smooth: return 0.5f;
    }
    return 0.67f;
}

void VideoQos::RttCalculator::update(uint32_t delay) {
    if (samples.size() >= kWindowSamples) samples.pop_front();
    samples.push_back(delay);
    baseline = baseline ? std::min(*baseline, delay) : delay;
    if (samples.size() < kWindowSamples) return;
    const size_t half = kWindowSamples / 2;
    const uint32_t older = *std::min_element(samples.begin(), samples.begin() + half);
    const uint32_t recent = *std::min_element(samples.begin() + half, samples.end());
    // A rising floor can be a growing queue. Allow 10 ms of probe granularity,
    // but wait for it to settle before forgetting the old baseline.
    if (recent > older + 10) return;
    const uint32_t rise = std::min(older, recent) - std::min(*baseline, std::min(older, recent));
    if (rise > 0)
        baseline = *baseline + std::clamp(rise / 2, 1u, kMaxIncreaseMs);
}

void VideoQos::Delay::add(uint32_t value) {
    if (history.size() >= 2) history.pop_front();
    history.push_back(value);
}

uint32_t VideoQos::Delay::average() const {
    if (history.empty()) return kDelayThresholdMs;
    const auto sum = std::accumulate(history.begin(), history.end(), uint64_t{0});
    const auto average = static_cast<uint32_t>(sum / history.size());
    const auto base = rtt.rtt().value_or(0);
    return average > base ? average - base : 0;
}

uint32_t VideoQos::Delay::limit_fps_change(uint32_t current, uint32_t proposed, uint32_t delay,
                                           bool bitrate_first, bool braked) {
    const auto base = rtt.rtt().value_or(0);
    const uint32_t excess = delay > base ? delay - base : 0;
    // A spike stays in the average for several samples; confirm congestion with fresh samples.
    if (samples_since_restore)
        *samples_since_restore = static_cast<uint8_t>(
            std::min<unsigned>(*samples_since_restore + 1, 255));
    if (excess < kDelayThresholdMs) {
        consecutive_bad = 0;
        fps_bad = 0;
        replies_after_bitrate_reduction.reset();
        good = std::min<uint8_t>(good + 1, 3);
        return recover(current, proposed);
    }
    consecutive_bad = std::min<uint8_t>(consecutive_bad + 1, 3);
    fps_bad = std::min<uint8_t>(fps_bad + 1, 3);
    if (replies_after_bitrate_reduction)
        *replies_after_bitrate_reduction = std::min<uint8_t>(
            *replies_after_bitrate_reduction + 1, 2);
    const bool guarded = samples_since_restore && *samples_since_restore <= kRestoreGuardSamples;
    const bool failed_restore = excess >= 600 && guarded;
    // A level that congests right after being restored is not the level to return to.
    if (guarded && (failed_restore || consecutive_bad >= 3)) {
        fps_before_congestion = current > current / 4 ? current - current / 4 : 1;
        samples_since_restore.reset();
    }
    // The timeout brake already reduced for the probe this reply answers.
    if (proposed >= current || braked) return current;
    // An extra second of delay cannot wait for another confirmation.
    if (!failed_restore && excess < 1000 &&
        (fps_bad < 3 ||
         (bitrate_first && replies_after_bitrate_reduction.value_or(0) < 2)))
        return current;
    // A fast restore probes capacity. Roll it back promptly if the queue grows
    // again, rather than waiting through another ordinary confirmation window.
    const uint32_t divisor = excess >= 1000 || failed_restore ? 2 : 5;
    on_reduction(current);
    const uint32_t step = std::max(current / divisor, 1u);
    return std::max(proposed, current - step);
}

uint32_t VideoQos::Delay::recover(uint32_t current, uint32_t proposed) {
    // Fresh low-delay replies permit recovery even while the average contains a
    // spike: a little at first, then back to the level held before congestion.
    const uint32_t gradual = current + std::max(current / 10, 1u);
    const bool has_level = fps_before_congestion && *fps_before_congestion > current;
    if ((good == 2 || good == 3) && has_level) {
        const uint32_t level = *fps_before_congestion;
        fps_before_congestion.reset();
        samples_since_restore = static_cast<uint8_t>(0);
        return std::max(proposed, level);
    }
    if (good == 3 && !has_level) {
        fps_before_congestion.reset();
        return std::max(proposed, current + std::max(current / 5, 2u));
    }
    return gradual;
}

uint32_t VideoQos::Delay::accelerate_startup(uint32_t current, uint32_t proposed, uint32_t cap,
                                             uint32_t delay, bool braked) {
    if (startup_good == kStartupEnded) return proposed;
    const auto base = rtt.rtt().value_or(0);
    const uint32_t excess = delay > base ? delay - base : 0;
    // A low-load sample does not establish capacity: require two clean replies
    // per step and abandon startup probing on the first sign of queue growth.
    if (braked || excess >= 50 || current >= cap) {
        startup_good = kStartupEnded;
        return proposed;
    }
    ++startup_good;
    if (startup_good < 2) return proposed;
    startup_good = 0;
    const uint32_t doubled = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(current) * 2, cap));
    const uint32_t accelerated = std::max(proposed, doubled);
    if (accelerated >= cap) startup_good = kStartupEnded;
    return accelerated;
}

void VideoQos::Delay::on_reduction(uint32_t current) {
    startup_good = kStartupEnded;
    good = 0;
    fps_bad = 0;
    if (!fps_before_congestion) fps_before_congestion = current;
}

bool VideoQos::Delay::needs_bitrate_reduction() const {
    // An outstanding video receipt is not probe jitter: it is application
    // delay already visible to the viewer. Waiting for a second tick past two
    // seconds lets the delivery window grow beyond the three-second latency
    // budget before the first rate cut. A single sub-two-second probe remains
    // jitter and never reaches this path.
    return consecutive_bad >= 2 || stall_ticks >= 1;
}

std::optional<float> VideoQos::Delay::ratio_reduction() const {
    if (!needs_bitrate_reduction()) return std::nullopt;
    // A delivery stall beyond two seconds is direct evidence that the current
    // target cannot drain the transport queue. An ordinary "unconfirmed"
    // 0.8 step leaves a 3 Mbps stream above a 1 Mbps bottleneck and produces
    // another multi-second backlog. Cut hard enough for the confirmed stall
    // while retaining the configured quality floor.
    if (stall_ticks) return 0.3f;
    const auto excess = average();
    const bool confirmed = consecutive_bad >= 3;
    if (excess < 200) return 0.95f;
    if (excess < 300) return 0.9f;
    if (excess < 500) return confirmed ? 0.7f : 0.85f;
    return confirmed ? 0.5f : 0.8f;
}

uint32_t VideoQos::fps_cap(const User& user) const {
    return user.fps ? user.fps : max_fps_;
}
uint32_t VideoQos::highest_fps() const {
    auto fps = max_fps_;
    for (const auto& [id, user] : users_) fps = std::min(fps, fps_cap(user));
    return std::clamp(fps, 1u, 120u);
}
VideoQos::Target VideoQos::latest_quality() const {
    Target target;
    uint64_t order = 0;
    for (const auto& [id, user] : users_) {
        if ((user.quality || user.custom_ratio) && user.quality_order > order) {
            target.quality = user.quality;
            target.custom_ratio = user.custom_ratio;
            order = user.quality_order;
        }
    }
    return target;
}
float VideoQos::target_ratio() const {
    const Target target = latest_quality();
    if (target.custom_ratio) return *target.custom_ratio;
    return target.quality ? quality_ratio(*target.quality) : default_ratio_;
}
float VideoQos::min_ratio() const {
    const Target target = latest_quality();
    const auto ratio = target_ratio();
    float minimum = 0.1f;
    float floor = 0.1f;
    // Presets keep a usable floor; a custom ratio keeps only the high-resolution
    // guard, as the reference does for Quality::Custom.
    if (target.custom_ratio) {
        // Nothing beyond the 0.1 guard.
    } else if ((target.quality && *target.quality == Quality::High) ||
               (!target.quality && ratio == 1.5f)) {
        minimum = 0.6f;
        floor = 0.2f;
    } else if ((target.quality && *target.quality == Quality::Balanced) ||
               (!target.quality && ratio == 0.67f)) {
        minimum = 0.335f;
    }
    if (bitrate_kbps_) {
        const auto one_mbps = std::max(ratio_ * 1000.0f / bitrate_kbps_, 0.1f);
        minimum = std::min(minimum, one_mbps);
    }
    return std::max(minimum, floor);
}
bool VideoQos::variable() const {
    return abr_ && std::all_of(displays_.begin(), displays_.end(),
        [](const auto& entry) { return entry.second.variable; });
}
bool VideoQos::can_reduce_bitrate() const {
    return variable() && !displays_.empty() && ratio_ > min_ratio() * 1.02f;
}
void VideoQos::open(uint64_t id, uint64_t now) {
    std::lock_guard lock(mutex_);
    if (users_.empty()) last_ratio_ = now;
    User user;
    user.joined_at = now;
    for (const auto& [peer_id, peer] : users_) {
        if (now >= peer.joined_at && now - peer.joined_at >= 1000 &&
            peer.delay.fps && peer.delay.good >= 2) {
            user.joining_fps = fps_;
            break;
        }
    }
    users_.try_emplace(id, user);
    adjust_fps(now);
}
void VideoQos::close(uint64_t id, uint64_t now) {
    std::lock_guard lock(mutex_);
    users_.erase(id);
    if (users_.empty()) {
        ratio_ = default_ratio_;
        fps_ = max_fps_;
        bitrate_kbps_ = 0;
        last_ratio_ = now;
        return;
    }
    adjust_fps(now);
    ratio_ = std::min(ratio_, target_ratio());
}
void VideoQos::options(uint64_t id, uint32_t fps, std::optional<Quality> quality, uint64_t now,
                       std::optional<float> custom_ratio) {
    std::lock_guard lock(mutex_);
    auto found = users_.find(id);
    if (found == users_.end()) return;
    found->second.fps = fps == 0 ? 0 : std::clamp(fps, 1u, 120u);
    if (quality) {
        found->second.quality = quality;
        found->second.custom_ratio.reset();
        found->second.quality_order = ++quality_order_;
        ratio_ = target_ratio();
    } else if (custom_ratio) {
        found->second.quality.reset();
        found->second.custom_ratio = std::clamp(*custom_ratio, 0.1f, 40.f);
        found->second.quality_order = ++quality_order_;
        ratio_ = target_ratio();
    }
    adjust_fps(now);
}
void VideoQos::adjust_fps(uint64_t now) {
    if (!adaptive_fps_) {
        // Exact-rate probes need the configured frame rate; production always
        // adapts, including when ABR only controls the quality ratio.
        fps_ = highest_fps();
        return;
    }
    if (users_.empty()) {
        fps_ = max_fps_;
        return;
    }
    uint32_t fps = std::numeric_limits<uint32_t>::max();
    for (const auto& [id, user] : users_)
        fps = std::min(fps, user.delay.fps.value_or(user.joining_fps.value_or(kInitFps)));
    // A new viewer must not restart the initial cohort's warm-up. Its own
    // feedback (including a stalled first receipt) still constrains the stream.
    for (const auto& [id, user] : users_)
        if (!user.joining_fps && now - user.joined_at < 1000) {
            fps = std::min(fps, kInitFps);
            break;
        }
    fps_ = std::clamp(fps, 1u, highest_fps());
}
void VideoQos::delay(uint64_t id, uint32_t value, uint64_t now) {
    std::lock_guard lock(mutex_);
    auto found = users_.find(id);
    if (found == users_.end() || !adaptive_fps_) return;
    auto& user = found->second;
    auto& delay = user.delay;
    const auto target = target_ratio();
    const uint32_t min_fps = target >= 1.5f ? 8 : target >= 0.67f ? 10 : 12;
    const uint32_t normal_fps = min_fps * 2;
    const bool bitrate_first = can_reduce_bitrate();
    const uint32_t sample = std::max(value, 10u);
    // The reply closes the outstanding probe, braked or not.
    delay.stall_ticks = 0;
    delay.last_stall_second = 0;
    const bool braked = delay.stall_reference_fps.has_value();
    delay.stall_reference_fps.reset();
    const auto old_average = delay.average();
    if (!braked) delay.rtt.update(sample);
    delay.add(sample);
    const auto average = std::max(delay.average(), 10u);
    const uint32_t cap = fps_cap(user);
    const uint32_t current = delay.fps.value_or(std::min(user.joining_fps.value_or(kInitFps), cap));
    uint32_t fps = current;
    if (average < 50) {
        auto step = fps < normal_fps ? 1u : 0u;
        if (++delay.quick_increase >= 3) { delay.quick_increase = 0; step = 5; }
        fps = std::max(min_fps, fps + step);
    } else if (average < 100) {
        fps = std::max(min_fps, fps + (average < old_average && fps < normal_fps ? 1u : 0u));
    } else if (average < kDelayThresholdMs) {
        fps = std::max(min_fps, fps);
    } else {
        const auto divided = static_cast<uint32_t>(
            std::ceil(fps / (average / static_cast<float>(kDelayThresholdMs))));
        if (average < 200) fps = std::max(min_fps, divided);
        else if (average < 300) fps = std::min(min_fps, divided);
        else if (average < 600) fps = kDelayThresholdMs * min_fps / average;
        else fps = std::min(kDelayThresholdMs * min_fps / average, divided);
    }
    delay.increase = average < kDelayThresholdMs ? delay.increase + 1 : 0;
    if (delay.increase >= 3) { delay.increase = 0; ++fps; }
    if (average > 50) delay.quick_increase = 0;
    // While the bitrate can still come down, the frame rate keeps its floor:
    // bitrate-targeted encoders do not send fewer bytes at fewer frames.
    if (bitrate_first) fps = std::max(fps, min_fps);
    fps = std::max(fps, std::min(kMinAutoFps, cap));
    fps = delay.limit_fps_change(current, fps, sample, bitrate_first, braked);
    fps = delay.accelerate_startup(current, fps, cap, sample, braked);
    const bool reduce_bitrate = bitrate_first && delay.needs_bitrate_reduction() &&
                                !delay.replies_after_bitrate_reduction;
    fps = std::clamp(fps, 1u, cap);
    const bool first = !delay.fps;
    delay.fps = fps;
    adjust_fps(now);
    // A viewer's first reply is one more trigger of the periodic adjustment and
    // keeps its cooldown: a viewer joining right after a cut must not spend the
    // other viewers' evidence a second time.
    if (first && now - last_ratio_ >= 3000) adjust_ratio(false, now);
    if (reduce_bitrate && now - last_ratio_ >= 3000) adjust_ratio(false, now);
}
void VideoQos::pending(uint64_t id, uint64_t elapsed, uint64_t now) {
    std::lock_guard lock(mutex_);
    auto found = users_.find(id);
    if (found == users_.end() || !adaptive_fps_) return;
    auto& user = found->second;
    auto& delay = user.delay;
    if (elapsed <= 2000) {
        delay.last_stall_second = 0;
        return;
    }
    // The caller samples every 500 ms; the reference ticks once per second.
    const auto second = static_cast<uint32_t>(
        std::min<uint64_t>(elapsed / 1000, std::numeric_limits<uint32_t>::max()));
    if (second <= delay.last_stall_second) return;
    delay.last_stall_second = second;
    delay.stall_ticks = static_cast<uint8_t>(std::min<unsigned>(delay.stall_ticks + 1, 255));
    delay.add(static_cast<uint32_t>(std::min<uint64_t>(elapsed, UINT32_MAX)));
    const uint32_t cap = fps_cap(user);
    uint32_t reference;
    if (delay.stall_reference_fps) {
        reference = *delay.stall_reference_fps;
    } else {
        reference = delay.fps.value_or(std::min(user.joining_fps.value_or(kInitFps), cap));
        delay.stall_reference_fps = reference;
        delay.on_reduction(reference);
    }
    // Halve for every second the probe stays out beyond the first: two seconds
    // halve, three quarter, and so on down to the floor.
    const uint32_t divisor = 1u << std::min<uint32_t>(second > 0 ? second - 1 : 0, 5);
    const uint32_t current = delay.fps.value_or(reference);
    const uint32_t floor = std::min(kMinAutoFps, cap);
    // The floor is a floor, not a lift: a target already below it stays.
    uint32_t fps = std::clamp(reference / divisor, floor, cap);
    fps = std::min(fps, current);
    delay.fps = fps;
    adjust_fps(now);
    // The periodic ratio tick has a three-second cooldown. An active delivery
    // stall cannot spend that budget before its first congestion action: apply
    // one bounded reduction as soon as the receipt is beyond two seconds.
    if (now - last_ratio_ >= 1000) adjust_ratio(false, now);
}
void VideoQos::adjust_ratio(bool dynamic, uint64_t now) {
    if (!variable() || users_.empty()) return;
    uint32_t maximum_delay = 0;
    for (const auto& [id, user] : users_) maximum_delay = std::max(maximum_delay, user.delay.average());
    // Each viewer judges its own delay; the stream takes the most conservative
    // step any viewer asks for.
    std::optional<float> reduction;
    for (const auto& [id, user] : users_) {
        const auto step = user.delay.ratio_reduction();
        if (step) reduction = reduction ? std::min(*reduction, *step) : *step;
    }
    if (!reduction && maximum_delay >= kDelayThresholdMs) {
        // Elevated but unconfirmed: no change, and no cooldown either, so a
        // confirmation on the next reply is acted on at once.
        for (auto& [id, display] : displays_) display.encoded = 0;
        return;
    }
    const auto target = target_ratio();
    const auto current = ratio_;
    std::optional<float> add_150kbps;
    if (bitrate_kbps_)
        add_150kbps = (bitrate_kbps_ + 150.0f) * current / bitrate_kbps_;
    const auto minimum = min_ratio();
    const auto maximum = target;
    float next = current;
    if (reduction) {
        next = current * *reduction;
    } else if (maximum_delay < 50) {
        if (dynamic) next = current * 1.15f;
    } else if (maximum_delay < 100) {
        if (dynamic) next = current * 1.1f;
    } else if (dynamic) {
        next = current * 1.05f;
    }
    // Limit quality increase rate for better stability.
    if (add_150kbps && next > *add_150kbps && *add_150kbps > current && current >= 0.5f)
        next = *add_150kbps;
    if (reduction) {
        for (auto& [id, user] : users_) {
            if (user.delay.needs_bitrate_reduction() && !user.delay.replies_after_bitrate_reduction) {
                // One outstanding probe may have started before the bitrate change.
                user.delay.replies_after_bitrate_reduction = static_cast<uint8_t>(
                    clamp_ratio(next, minimum, maximum) < current ? 0 : 2);
            }
        }
    }
    ratio_ = clamp_ratio(next, minimum, maximum);
    for (auto& [id, display] : displays_) display.encoded = 0;
    last_ratio_ = now;
}
void VideoQos::tick(uint64_t now) {
    adjust_fps(now);
    if (!variable()) {
        ratio_ = target_ratio();
        return;
    }
    if (now - last_ratio_ < 3000) return;
    // The reference feeds send counters to its periodic adjustment once a
    // second. An elevated but unconfirmed delay returns without spending the
    // cooldown, so re-check on that same cadence instead of every service tick;
    // otherwise the dynamic window shrinks from about a second to one tick.
    if (now - last_ratio_check_ < 1000) return;
    last_ratio_check_ = now;
    bool dynamic = false;
    for (const auto& [id, display] : displays_) dynamic = dynamic || display.encoded >= 6;
    adjust_ratio(dynamic, now);
}
void VideoQos::display(uint64_t id, uint32_t encoded, uint32_t bitrate, bool variable_rate, uint64_t now) {
    std::lock_guard lock(mutex_);
    auto& display = displays_[id];
    display.encoded += encoded;
    display.variable = variable_rate;
    bitrate_kbps_ = bitrate;
    tick(now);
}
void VideoQos::remove_display(uint64_t id) {
    std::lock_guard lock(mutex_);
    displays_.erase(id);
}
VideoQos::State VideoQos::state(uint64_t now) {
    std::lock_guard lock(mutex_);
    tick(now);
    return {fps_, ratio_};
}
} // namespace chast
