#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include "chast/audio.h"

namespace chast {
// System sound is host-wide, independent of display subscriptions. The last
// connection releases the endpoint; selecting another display only changes
// video. Slow readers have private queues bounded to 25 packets and 500 ms of
// audio age, with stale drops reported separately from queue overflow.
class SharedLoopback {
public:
    using Factory = std::function<std::unique_ptr<ILoopbackCapture>(std::string*)>;
    std::unique_ptr<ILoopbackCapture> subscribe(Factory factory, std::string* error);
    const std::shared_ptr<AudioReference> reference = std::make_shared<AudioReference>();
private:
    struct Source;
    class Reader;
    std::mutex mutex_;
    std::weak_ptr<Source> source_;
};
} // namespace chast
