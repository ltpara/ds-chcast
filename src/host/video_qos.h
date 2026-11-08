#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

namespace chast {

// Host-wide video policy, shared by every display service. Time is supplied
// by callers so the same delay traces can be verified without real sleeps.
// Rules follow rustdesk-ref/src/server/video_qos.rs (7423dced).
class VideoQos {
public:
    enum class Quality { High, Balanced, Smooth };
    struct State {
        uint32_t fps;
        float ratio;
    };
    explicit VideoQos(uint32_t max_fps = 30, float default_ratio = 0.67f,
                      bool adaptive = true);
    void open(uint64_t id, uint64_t now_ms);
    void close(uint64_t id, uint64_t now_ms);
    void options(uint64_t id, uint32_t fps, std::optional<Quality> quality, uint64_t now_ms);
    void delay(uint64_t id, uint32_t delay_ms, uint64_t now_ms);
    void pending(uint64_t id, uint64_t elapsed_ms, uint64_t now_ms);
    void display(uint64_t id, uint32_t encoded, uint32_t bitrate_kbps, bool variable,
                 uint64_t now_ms);
    void remove_display(uint64_t id);
    State state(uint64_t now_ms);
    static float quality_ratio(Quality quality);

private:
    struct Delay {
        std::deque<uint32_t> samples, recent;
        std::optional<uint32_t> minimum, smoothed, fps;
        uint32_t quick_increase = 0, increase = 0;
        bool pending = false;
        void add(uint32_t value);
        uint32_t average() const;
    };
    struct User {
        uint32_t fps = 0;
        std::optional<Quality> quality;
        uint64_t quality_order = 0;
        Delay delay;
    };
    struct Display { uint32_t encoded = 0; bool variable = true; };
    uint32_t highest_fps() const;
    std::optional<Quality> latest_quality() const;
    float target_ratio() const;
    bool variable() const;
    void adjust_fps(uint64_t now_ms);
    void adjust_ratio(bool dynamic, uint64_t now_ms);
    void tick(uint64_t now_ms);
    std::mutex mutex_;
    std::map<uint64_t, User> users_;
    std::map<uint64_t, Display> displays_;
    uint32_t max_fps_, fps_, bitrate_kbps_ = 0;
    float default_ratio_, ratio_;
    bool adaptive_;
    uint64_t latest_open_ = 0, last_ratio_ = 0, quality_order_ = 0;
};
} // namespace chast
