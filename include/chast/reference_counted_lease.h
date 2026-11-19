#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>

namespace chast {

// Shares one process-level request among independent session owners.
class ReferenceCountedLease {
public:
    ReferenceCountedLease(std::function<bool()> acquire,
                         std::function<bool()> release)
        : acquire_(std::move(acquire)), release_(std::move(release)) {}

    ~ReferenceCountedLease() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ != 0) release_();
    }

    ReferenceCountedLease(const ReferenceCountedLease&) = delete;
    ReferenceCountedLease& operator=(const ReferenceCountedLease&) = delete;

    bool acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == std::numeric_limits<std::size_t>::max()) return false;
        if (count_ == 0 && !acquire_()) return false;
        ++count_;
        return true;
    }

    bool release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) return false;
        if (--count_ == 0) return release_();
        return true;
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    std::function<bool()> acquire_;
    std::function<bool()> release_;
    mutable std::mutex mutex_;
    std::size_t count_ = 0;
};

}  // namespace chast
