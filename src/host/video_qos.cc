#include "host/video_qos.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace chast {
VideoQos::VideoQos(uint32_t max_fps, float default_ratio, bool adaptive)
    : max_fps_(std::clamp(max_fps ? max_fps : 30u, 1u, 120u)), fps_(max_fps_),
      default_ratio_(std::clamp(default_ratio, 0.1f, 40.f)), ratio_(default_ratio_),
      adaptive_(adaptive) {}

float VideoQos::quality_ratio(Quality quality) {
    switch (quality) {
        case Quality::High: return 1.5f;
        case Quality::Balanced: return 0.67f;
        case Quality::Smooth: return 0.5f;
    }
    return 0.67f;
}
void VideoQos::Delay::add(uint32_t value) {
    minimum = minimum ? std::min(*minimum, value) : value;
    if (samples.size() >= 60) samples.pop_front();
    samples.push_back(value);
    if (samples.size() == 60)
        smoothed = static_cast<uint32_t>((static_cast<uint64_t>(*minimum) +
                    *std::min_element(samples.begin(), samples.end())) / 2);
    // The reference retains three recent samples (it removes when len > 2).
    if (recent.size() > 2) recent.pop_front();
    recent.push_back(value);
}
uint32_t VideoQos::Delay::average() const {
    if (recent.empty()) return 150;
    const auto average = static_cast<uint32_t>(
        std::accumulate(recent.begin(), recent.end(), uint64_t{0}) / recent.size());
    const auto rtt = smoothed ? smoothed : samples.size() >= 10 ? minimum : std::nullopt;
    return rtt && average > *rtt ? average - *rtt : average;
}
uint32_t VideoQos::highest_fps() const {
    auto fps = max_fps_;
    for (const auto& [id, user] : users_) if (user.fps) fps = std::min(fps, user.fps);
    return std::clamp(fps, 1u, 120u);
}
std::optional<VideoQos::Quality> VideoQos::latest_quality() const {
    std::optional<Quality> quality;
    uint64_t order = 0;
    for (const auto& [id, user] : users_) if (user.quality && user.quality_order > order) {
        quality = user.quality;
        order = user.quality_order;
    }
    return quality;
}
float VideoQos::target_ratio() const {
    auto quality = latest_quality();
    return quality ? quality_ratio(*quality) : default_ratio_;
}
bool VideoQos::variable() const {
    return adaptive_ && std::all_of(displays_.begin(), displays_.end(),
        [](const auto& entry) { return entry.second.variable; });
}
void VideoQos::open(uint64_t id, uint64_t now) {
    std::lock_guard lock(mutex_);
    if (users_.empty()) last_ratio_ = now;
    users_.try_emplace(id);
    latest_open_ = now;
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
void VideoQos::options(uint64_t id, uint32_t fps, std::optional<Quality> quality, uint64_t now) {
    std::lock_guard lock(mutex_);
    auto found = users_.find(id);
    if (found == users_.end()) return;
    found->second.fps = fps == 0 ? 0 : std::clamp(fps, 1u, 120u);
    if (quality) {
        found->second.quality = quality;
        found->second.quality_order = ++quality_order_;
        ratio_ = target_ratio();
    }
    adjust_fps(now);
}
void VideoQos::adjust_fps(uint64_t now) {
    auto fps = highest_fps();
    if (adaptive_) {
        for (const auto& [id, user] : users_) {
            fps = std::min(fps, user.delay.fps.value_or(15));
            if (user.delay.pending) fps = std::min(fps, 2u);
        }
        if (now - latest_open_ < 1000) fps = std::min(fps, 15u);
    }
    fps_ = std::clamp(fps, 1u, highest_fps());
}
void VideoQos::delay(uint64_t id, uint32_t value, uint64_t now) {
    std::lock_guard lock(mutex_);
    auto found = users_.find(id);
    if (found == users_.end() || !adaptive_) return;
    auto& delay = found->second.delay;
    const auto old = delay.average();
    delay.add(std::max(value, 10u));
    delay.pending = false;
    const auto average = std::max(delay.average(), 10u);
    const auto target = target_ratio();
    const uint32_t minimum = target >= 1.5f ? 8 : target >= 0.67f ? 10 : 12;
    const uint32_t normal = minimum * 2;
    auto fps = fps_;
    if (average < 50) {
        auto step = fps < normal ? 1u : 0u;
        if (++delay.quick_increase >= 3) { delay.quick_increase = 0; step = 5; }
        fps = std::max(minimum, fps + step);
    } else if (average < 100) {
        fps = std::max(minimum, fps + (average < old && fps < normal ? 1u : 0u));
    } else if (average < 150) {
        fps = std::max(minimum, fps);
    } else {
        const auto divided = static_cast<uint32_t>(std::ceil(fps * 150.0 / average));
        if (average < 200) fps = std::max(minimum, divided);
        else if (average < 300) fps = std::min(minimum, divided);
        else if (average < 600) fps = 150 * minimum / average;
        else fps = std::min(150 * minimum / average, divided);
    }
    delay.increase = average < 150 ? delay.increase + 1 : 0;
    if (delay.increase >= 3) { delay.increase = 0; ++fps; }
    if (average > 50) delay.quick_increase = 0;
    const bool first = !delay.fps;
    delay.fps = std::clamp(fps, 1u, highest_fps());
    adjust_fps(now);
    if (first) adjust_ratio(false, now);
}
void VideoQos::pending(uint64_t id, uint64_t elapsed, uint64_t now) {
    std::lock_guard lock(mutex_);
    auto found = users_.find(id);
    if (found == users_.end() || !adaptive_) return;
    auto& delay = found->second.delay;
    delay.pending = elapsed > 2000;
    if (delay.pending) delay.add(static_cast<uint32_t>(std::min<uint64_t>(elapsed, UINT32_MAX)));
    adjust_fps(now);
}
void VideoQos::adjust_ratio(bool dynamic, uint64_t now) {
    if (!variable() || users_.empty()) return;
    uint32_t maximum_delay = 0;
    for (const auto& [id, user] : users_) maximum_delay = std::max(maximum_delay, user.delay.average());
    float minimum = 0.1f;
    const auto quality = latest_quality();
    const auto target = target_ratio();
    if ((quality && *quality == Quality::High) || (!quality && target == 1.5f)) minimum = 0.6f;
    else if ((quality && *quality == Quality::Balanced) || (!quality && target == 0.67f)) minimum = 0.335f;
    const auto floor = minimum == 0.6f ? 0.2f : 0.1f;
    if (bitrate_kbps_) minimum = std::min(minimum, std::max(ratio_ * 1000 / bitrate_kbps_, 0.1f));
    minimum = std::max(minimum, floor);
    float next = ratio_;
    if (maximum_delay < 150) {
        if (dynamic) next *= maximum_delay < 50 ? 1.15f : maximum_delay < 100 ? 1.1f : 1.05f;
    } else {
        next *= maximum_delay < 200 ? 0.95f : maximum_delay < 300 ? 0.9f : maximum_delay < 500 ? 0.85f : 0.8f;
    }
    if (bitrate_kbps_ && ratio_ >= 0.5f)
        next = std::min(next, ratio_ * (1.0f + 150.0f / bitrate_kbps_));
    ratio_ = std::clamp(next, std::min(minimum, target), target);
    last_ratio_ = now;
}
void VideoQos::tick(uint64_t now) {
    adjust_fps(now);
    if (!variable()) { ratio_ = target_ratio(); return; }
    if (now - last_ratio_ < 3000) return;
    bool dynamic = false;
    for (auto& [id, display] : displays_) {
        dynamic = dynamic || display.encoded >= 6;
        display.encoded = 0;
    }
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
