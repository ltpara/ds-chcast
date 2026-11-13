#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

namespace chast {

// Host-wide video policy, shared by every display service. Time is supplied
// by callers so the same delay traces can be verified without real sleeps.
// Rules follow rustdesk-ref/src/server/video_qos.rs.
class VideoQos {
public:
    enum class Quality { High, Balanced, Smooth };
    struct State {
        uint32_t fps;
        float ratio;
    };
    // abr matches RustDesk's enable-abr: it gates only the quality ratio.
    // Delay-driven FPS adaptation stays on unless adaptive_fps is turned off;
    // only probes that assert exact configured rates need that.
    explicit VideoQos(uint32_t max_fps = 30, float default_ratio = 0.67f,
                      bool abr = true, bool adaptive_fps = true);
    void open(uint64_t id, uint64_t now_ms);
    void close(uint64_t id, uint64_t now_ms);
    void options(uint64_t id, uint32_t fps, std::optional<Quality> quality, uint64_t now_ms,
                 std::optional<float> custom_ratio = std::nullopt);
    void delay(uint64_t id, uint32_t delay_ms, uint64_t now_ms);
    void pending(uint64_t id, uint64_t elapsed_ms, uint64_t now_ms);
    void display(uint64_t id, uint32_t encoded, uint32_t bitrate_kbps, bool variable,
                 uint64_t now_ms);
    void remove_display(uint64_t id);
    State state(uint64_t now_ms);
    static float quality_ratio(Quality quality);

private:
    struct RttCalculator {
        static constexpr size_t kWindowSamples = 20;
        static constexpr uint32_t kMaxIncreaseMs = 50;
        std::deque<uint32_t> samples;
        std::optional<uint32_t> baseline;
        void update(uint32_t delay);
        std::optional<uint32_t> rtt() const { return baseline; }
    };
    struct Delay {
        // The reference keeps the last two replies and subtracts the learned
        // path baseline from their average.
        std::deque<uint32_t> history;
        std::optional<uint32_t> fps;
        RttCalculator rtt;
        uint32_t quick_increase = 0, increase = 0;
        uint8_t consecutive_bad = 0, fps_bad = 0, good = 0;
        std::optional<uint8_t> replies_after_bitrate_reduction;
        std::optional<uint32_t> fps_before_congestion;
        std::optional<uint8_t> samples_since_restore;
        std::optional<uint32_t> stall_reference_fps;
        uint8_t stall_ticks = 0;
        uint32_t last_stall_second = 0;
        uint8_t startup_good = 0; // u8::MAX permanently ends startup acceleration
        void add(uint32_t value);
        uint32_t average() const;
        uint32_t limit_fps_change(uint32_t current, uint32_t proposed, uint32_t delay,
                                  bool bitrate_first, bool braked);
        uint32_t recover(uint32_t current, uint32_t proposed);
        uint32_t accelerate_startup(uint32_t current, uint32_t proposed, uint32_t cap,
                                    uint32_t delay, bool braked);
        void on_reduction(uint32_t current);
        bool needs_bitrate_reduction() const;
        std::optional<float> ratio_reduction() const;
    };
    struct User {
        uint32_t fps = 0;
        std::optional<Quality> quality;
        // Set by a custom numeric quality; the newest vote of either kind
        // wins, so a preset clears this and vice versa.
        std::optional<float> custom_ratio;
        uint64_t quality_order = 0;
        uint64_t joined_at = 0;
        // Late viewers inherit a proven shared rate. Only the initial cohort
        // needs the conservative 15 FPS start and its one-second guard.
        std::optional<uint32_t> joining_fps;
        Delay delay;
    };
    struct Display { uint32_t encoded = 0; bool variable = true; };
    struct Target {
        std::optional<Quality> quality;
        std::optional<float> custom_ratio;
    };
    uint32_t fps_cap(const User& user) const;
    uint32_t highest_fps() const;
    Target latest_quality() const;
    float target_ratio() const;
    float min_ratio() const;
    bool variable() const;
    bool can_reduce_bitrate() const;
    void adjust_fps(uint64_t now_ms);
    void adjust_ratio(bool dynamic, uint64_t now_ms);
    void tick(uint64_t now_ms);
    std::mutex mutex_;
    std::map<uint64_t, User> users_;
    std::map<uint64_t, Display> displays_;
    uint32_t max_fps_, fps_, bitrate_kbps_ = 0;
    float default_ratio_, ratio_;
    bool abr_;
    bool adaptive_fps_;
    uint64_t last_ratio_ = 0, last_ratio_check_ = 0, quality_order_ = 0;
};
} // namespace chast
